# The defaults use the upstream origins so the image builds anywhere. Pass
# --build-arg BASE_IMAGE=docker.1ms.run/library/debian:trixie,
# --build-arg APT_MIRROR=mirrors.huaweicloud.com and
# --build-arg GITHUB_PROXY=https://gh-proxy.com when those are faster.
ARG BASE_IMAGE=debian:trixie
FROM ${BASE_IMAGE}
ARG APT_MIRROR=deb.debian.org
ARG GITHUB_PROXY=
ARG VCPKG_TAG=2026.10.16
ARG SOURCEWARE_MIRROR=
RUN sed -i "s|deb.debian.org|${APT_MIRROR}|g" /etc/apt/sources.list.d/debian.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
       build-essential cmake ninja-build pkg-config git curl ca-certificates zip unzip tar \
       autoconf automake autoconf-archive autotools-dev libtool bison flex gettext autopoint python3 \
       clang llvm libbpf-dev linux-libc-dev bpftool make clang-format openssl \
       gcc-aarch64-linux-gnu libc6-dev-arm64-cross linux-libc-dev-arm64-cross \
    && rm -rf /var/lib/apt/lists/*
ENV GITHUB_PROXY=${GITHUB_PROXY} \
    SOURCEWARE_MIRROR=${SOURCEWARE_MIRROR} \
    VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_OVERLAY_PORTS=/opt/etrace-vcpkg/ports \
    VCPKG_OVERLAY_TRIPLETS=/opt/etrace-vcpkg/triplets \
    X_VCPKG_ASSET_SOURCES="x-script,/usr/local/bin/vcpkg-fetch {url} {dst};x-block-origin"
COPY build/vcpkg-fetch.sh /usr/local/bin/vcpkg-fetch
RUN chmod +x /usr/local/bin/vcpkg-fetch \
    && git clone --depth 1 --branch ${VCPKG_TAG} "${GITHUB_PROXY:+${GITHUB_PROXY}/}https://github.com/microsoft/vcpkg.git" /opt/vcpkg \
    && . /opt/vcpkg/scripts/vcpkg-tool-metadata.txt \
    && case "$(uname -m)" in aarch64) asset=vcpkg-glibc-arm64 ;; *) asset=vcpkg-glibc ;; esac \
    && vcpkg-fetch "https://github.com/microsoft/vcpkg-tool/releases/download/${VCPKG_TOOL_RELEASE_TAG}/${asset}" /opt/vcpkg/vcpkg \
    && chmod +x /opt/vcpkg/vcpkg
COPY build/vcpkg /opt/etrace-vcpkg
COPY vcpkg.json /deps/vcpkg.json
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    cd /deps && { /opt/vcpkg/vcpkg install --x-install-root=/opt/vcpkg_installed \
       || { find /opt/vcpkg/buildtrees -maxdepth 2 -name '*.log' -exec tail -n 40 {} +; exit 1; }; } \
    && rm -rf /opt/vcpkg/buildtrees /opt/vcpkg/downloads /opt/vcpkg/packages
ENV VCPKG_INSTALLED_DIR=/opt/vcpkg_installed
WORKDIR /src
