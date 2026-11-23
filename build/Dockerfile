ARG BUILDER_IMAGE=etrace-cpp-builder
ARG RUNTIME_IMAGE=debian:trixie-slim
FROM ${BUILDER_IMAGE} AS build
ARG VERSION=dev
COPY . /src
RUN cmake -S /src -B /build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
      -DETRACE_BUILD_TESTS=OFF -DETRACE_VERSION=${VERSION} \
    && ninja -C /build etrace && strip /build/etrace

FROM ${RUNTIME_IMAGE}
ARG APT_MIRROR=deb.debian.org
RUN sed -i "s|deb.debian.org|${APT_MIRROR}|g" /etc/apt/sources.list.d/debian.sources \
    && apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/etrace /usr/local/bin/etrace
COPY LICENSE NOTICE /usr/share/doc/etrace/
ENTRYPOINT ["/usr/local/bin/etrace"]
