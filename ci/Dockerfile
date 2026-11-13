# CPU-only Linux verification toolchain. No runtime services or published ports.
FROM golang:1.27.1-bookworm@sha256:8d48e12ec56735e9358640898b9d9b9fcca110612ed8a5567438c0a1baa24e66 AS go
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential g++-14 clang-18 libclang-rt-18-dev llvm-18 lld-18 cmake ninja-build \
    git ca-certificates curl zip unzip tar pkg-config \
    autoconf automake libtool bison gperf perl nasm python3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=go /usr/local/go /usr/local/go
ENV PATH="/usr/local/go/bin:${PATH}"
RUN git init /opt/vcpkg \
    && git -C /opt/vcpkg remote add origin https://github.com/microsoft/vcpkg.git \
    && git -C /opt/vcpkg fetch --depth 1 origin 3d22b64585ca026e881728fd87e541c23a611a43 \
    && git -C /opt/vcpkg checkout --detach FETCH_HEAD \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && chown -R 1000:1000 /opt/vcpkg
ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_DISABLE_METRICS=1
ENV HOME=/tmp/connectcpp-home
USER 1000:1000
WORKDIR /workspace
