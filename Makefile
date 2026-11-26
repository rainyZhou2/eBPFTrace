HOST_ARCH ?= $(shell uname -m | sed -e 's/x86_64/amd64/' -e 's/aarch64/arm64/')
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
IMAGE ?= etrace
BUILDER ?= etrace-cpp-builder
BUILDER_BUILD_ARGS ?=
BUILD_VOLUME ?= etrace-build
TEST_IMAGE ?= docker.1ms.run/library/debian:trixie
CLICKHOUSE_IMAGE ?= docker.1ms.run/clickhouse/clickhouse-server:24.8
CMAKE_FLAGS ?=

# Builds run inside the builder image; the build tree lives in a named
# volume because bind mounts from macOS are slow.
IN_BUILDER = docker run --rm -v $(CURDIR):/src -v $(BUILD_VOLUME):/build $(BUILDER)
CONFIGURE = cmake -S /src -B /build -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
	-DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
	-DETRACE_VERSION=$(VERSION) $(CMAKE_FLAGS)

.PHONY: all builder build test test-clickhouse test-kernel kernel-bin scenarios e2e e2e-scenario image image-push fmt fmt-check bpf-ide-headers ide clean

all: build

builder:
	docker buildx build --load $(BUILDER_BUILD_ARGS) -f build/cpp-builder.Dockerfile -t $(BUILDER) .

build:
	@mkdir -p bin
	$(IN_BUILDER) sh -c '$(CONFIGURE) >/dev/null && ninja -C /build'
	docker run --rm -v $(BUILD_VOLUME):/build -v $(CURDIR)/bin:/out $(BUILDER) \
		cp /build/etrace /out/etrace-linux-$(HOST_ARCH)

test: scenarios
	$(IN_BUILDER) sh -c '$(CONFIGURE) >/dev/null && ninja -C /build etrace_tests && /build/etrace_tests'

# Copies the BPF test binary out of the build volume for test/kernel/qemu.
kernel-bin:
	$(IN_BUILDER) sh -c '$(CONFIGURE) >/dev/null && ninja -C /build etrace_kernel_tests'
	@mkdir -p bin
	docker run --rm -v $(BUILD_VOLUME):/build -v $(CURDIR)/bin:/out $(BUILDER) \
		cp /build/etrace_kernel_tests /out/etrace_kernel_tests

# Runs the ClickHouse driver test against a throwaway server.
test-clickhouse:
	-docker network create etrace-test >/dev/null 2>&1
	-docker rm -f etrace-ch >/dev/null 2>&1
	docker run -d --name etrace-ch --network etrace-test -e CLICKHOUSE_USER=etrace -e CLICKHOUSE_PASSWORD=etrace $(CLICKHOUSE_IMAGE) >/dev/null
	ok=; for i in $$(seq 1 120); do \
		if docker exec etrace-ch clickhouse-client --user etrace --password etrace -q 'SELECT 1' >/dev/null 2>&1; then ok=1; break; fi; \
		sleep 1; \
	done; \
	if [ -z "$$ok" ]; then docker logs etrace-ch; docker rm -f etrace-ch >/dev/null; exit 1; fi
	docker run --rm --network etrace-test -e ETRACE_CLICKHOUSE_ADDR=etrace-ch:9000 \
		-e ETRACE_CLICKHOUSE_USER=etrace -e ETRACE_CLICKHOUSE_PASSWORD=etrace \
		-v $(CURDIR):/src -v $(BUILD_VOLUME):/build $(BUILDER) \
		sh -c '$(CONFIGURE) >/dev/null && ninja -C /build etrace_tests && /build/etrace_tests --gtest_filter=ClickHouse.*'; \
	status=$$?; docker rm -f etrace-ch >/dev/null; exit $$status

scenarios:
	$(IN_BUILDER) sh -c '\
		make -C sdk/c CC=clang && cp sdk/c/libetrace_ctx.so test/scenario/ && make -C sdk/c clean && \
		clang -O2 -static -pthread -o test/scenario/lockchain-$(HOST_ARCH) test/scenario/lockchain.c && \
		if [ "$(HOST_ARCH)" = amd64 ]; then \
			aarch64-linux-gnu-gcc -O2 -static -pthread -o test/scenario/lockchain-arm64 test/scenario/lockchain.c; \
		fi && \
		clang -O2 -fomit-frame-pointer -static -pthread -o test/scenario/nofp-$(HOST_ARCH) test/scenario/nofp.c && \
		clang -O2 -pthread -Isdk/c -o test/scenario/traced-$(HOST_ARCH) test/scenario/traced.c \
			-Ltest/scenario -letrace_ctx -Wl,-rpath,\$$ORIGIN && \
		cd test/scenario && \
		clang -O2 -fPIC -shared -DFAKE_CUDA -Wl,-soname,libcuda.so.1 -o libcuda.so.1 fakecuda.c && \
		clang -O2 -fPIC -shared -DFAKE_NCCL -Wl,-soname,libnccl.so.2 -o libnccl.so.2 fakecuda.c && \
		clang -O2 -fPIC -shared -DFAKE_CUDART -Wl,-soname,libcudart.so.12 -o libcudart.so.12 fakecuda.c \
			-L. -l:libcuda.so.1 -Wl,-rpath,\$$ORIGIN && \
		clang -O2 -o gpu-$(HOST_ARCH) gpu.c -L. -l:libcudart.so.12 -l:libcuda.so.1 -l:libnccl.so.2 \
			-Wl,-rpath,\$$ORIGIN'

# Loads the BPF programs in a privileged container in the host PID
# namespace. Binaries are copied into the container because uprobes cannot
# attach to files bind-mounted from macOS.
test-kernel: scenarios
	$(IN_BUILDER) sh -c '$(CONFIGURE) >/dev/null && ninja -C /build etrace_kernel_tests'
	docker run --rm --privileged --pid=host -v $(BUILD_VOLUME):/build:ro -v $(CURDIR)/test/scenario:/s:ro \
		-e ETRACE_SCENARIO_LOCKCHAIN=/scen/lockchain-$(HOST_ARCH) -e ETRACE_SCENARIO_TRACED=/scen/traced-$(HOST_ARCH) \
		-e ETRACE_SCENARIO_NOFP=/scen/nofp-$(HOST_ARCH) -e ETRACE_SCENARIO_GPU=/scen/gpu-$(HOST_ARCH) $(TEST_IMAGE) \
		sh -c 'cp -r /s /scen && cp /build/etrace_kernel_tests /kt && /kt'

e2e-scenario:
	$(IN_BUILDER) clang -O2 -Wall -Wextra -Werror -fPIC -shared -pthread \
		-o /src/test/scenario/libetrace_load_cache.so /src/test/e2e/stack/load_cache.c

e2e: build scenarios e2e-scenario
	HOST_ARCH=$(HOST_ARCH) test/e2e/stack/run.sh

image:
	docker buildx build --platform linux/$(HOST_ARCH) --load --build-arg VERSION=$(VERSION) -f build/Dockerfile -t $(IMAGE):$(VERSION) .
	docker tag $(IMAGE):$(VERSION) $(IMAGE):latest

image-push:
	docker buildx build --platform linux/amd64,linux/arm64 --push --build-arg VERSION=$(VERSION) -f build/Dockerfile -t $(IMAGE):$(VERSION) .

fmt:
	$(IN_BUILDER) sh -c 'cd /src && find src test -name "*.cpp" -o -name "*.h" | xargs clang-format -i'

fmt-check:
	$(IN_BUILDER) sh -c 'cd /src && find src test -name "*.cpp" -o -name "*.h" | xargs clang-format --dry-run -Werror'

# Copies the Linux UAPI and libbpf headers out of the builder image so
# clangd can check the BPF sources on hosts without them (see .clangd).
bpf-ide-headers:
	rm -rf build/bpf-include && mkdir -p build/bpf-include
	docker run --rm -v $(CURDIR)/build/bpf-include:/out $(BUILDER) sh -c '\
		cp -rL /usr/include/bpf /usr/include/asm-generic /out/ && \
		mkdir -p /out/linux && cp -L /usr/include/linux/*.h /out/linux/ && \
		cp -rL /usr/include/$$(uname -m)-linux-gnu/asm /out/asm'

# Copies the compilation database, the generated protobuf headers and the
# vcpkg headers out of the build volume so clangd can check the C++ sources
# on the host (see .clangd). Run after make build.
ide:
	rm -rf build/ide && mkdir -p build/ide
	docker run --rm -v $(BUILD_VOLUME):/build -v $(CURDIR)/build/ide:/out $(BUILDER) sh -c '\
		cp -r /build/gen /out/gen && cp -rL /opt/vcpkg_installed/*/include /out/include && \
		sed -e "s|/opt/vcpkg_installed/[^ ]*/include|$(CURDIR)/build/ide/include|g" \
		    -e "s|/build/gen|$(CURDIR)/build/ide/gen|g" -e "s|/src/|$(CURDIR)/|g" \
		    -e "s|\"directory\": \"/build\"|\"directory\": \"$(CURDIR)/build/ide\"|" \
		    /build/compile_commands.json > /out/compile_commands.json'

clean:
	rm -rf bin
	-docker volume rm $(BUILD_VOLUME)
