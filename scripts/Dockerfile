# syntax=docker/dockerfile:1
#
# Build a fully static ds-service binary against musl.
#
# The resulting executable has no dynamic dependencies at all,
# so the final stage is `scratch`
# and the image contains nothing but the binary.
#
#     docker build -f scripts/Dockerfile -t ds-service:static .
#     docker run --rm -p 5051:5051 ds-service:static
#
# To extract the binary instead of running it:
#
#     docker build -f scripts/Dockerfile --output type=local,dest=./dist .
#
# ConanCenter publishes no musl binaries,
# so every C++ dependency — gRPC, protobuf, Abseil, OpenSSL, re2, zlib, c-ares —
# is compiled from source.
# Expect the first build to take a long time.
#
# The `syntax` line above is required:
# the profile below is written with a heredoc.

FROM alpine:3.24.1 AS build

# Alpine 3.24 ships gcc 15.2, cmake 4.2 and musl 1.2.6,
# which covers the C++23 and CMake >= 4.0 requirements.
# perl and linux-headers are needed by the OpenSSL build gRPC pulls in,
# python3-dev by any Conan dependency without a musllinux wheel,
# and bash to source Conan's generated conanbuild.sh.
RUN apk add --no-cache \
        g++ musl-dev linux-headers make cmake git bash \
        perl python3 python3-dev py3-pip \
    && pip install --break-system-packages --no-cache-dir conan

ENV CONAN_HOME=/opt/conan_home

# compiler.version tracks the gcc Alpine ships,
# and cmake is declared platform-provided
# because conanfile.py tool_requires it
# while ConanCenter's cmake package merely repackages Kitware's glibc binaries,
# which cannot execute here.
# Conan does not check the platform tool's real version;
# it only has to satisfy CMakeLists.txt's `cmake_minimum_required(VERSION 4.0)`.
RUN mkdir -p "$CONAN_HOME/profiles" \
    && cat > "$CONAN_HOME/profiles/default" <<'EOF'
[settings]
arch=x86_64
build_type=Release
compiler=gcc
compiler.cppstd=20
compiler.libcxx=libstdc++11
compiler.version=15
os=Linux

[platform_tool_requires]
cmake/4.3.3

[options]
grpc/*:codegen=True
grpc/*:csharp_plugin=False
grpc/*:node_plugin=False
grpc/*:objective_c_plugin=False
grpc/*:php_plugin=False
grpc/*:ruby_plugin=False
grpc/*:python_plugin=False
EOF

WORKDIR /src

# The recipe is copied on its own first
# so that editing the sources does not invalidate the dependency build.
COPY conanfile.py ./
RUN conan install . --build=missing

# These are exactly the paths conanfile.py exports.
COPY CMakeLists.txt ./
COPY cpp/ ./cpp/
COPY misc/ ./misc/

SHELL ["/bin/bash", "-c"]

# -static is applied here rather than through the profile's exelinkflags
# so that it only affects this executable;
# forcing it on every dependency's configure-time link checks breaks some recipes.
#
# musl takes its default *thread* stack size from PT_GNU_STACK,
# and its built-in 128 KiB default is tight for gRPC's worker threads,
# hence -z stack-size.
RUN . build/Release/generators/conanbuild.sh \
    && cmake -S . -B build/Release \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-Wall -Wextra" \
        -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,-z,stack-size=1048576" \
        -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake \
    && cmake --build build/Release --parallel \
    && strip build/Release/ds-service

# Fail the build rather than ship something that silently kept an interpreter.
RUN if readelf -l build/Release/ds-service | grep -q INTERP; then \
        echo "ERROR: ds-service is not statically linked" >&2; \
        exit 1; \
    fi

# The server only ever uses grpc::InsecureServerCredentials(),
# so no CA bundle is needed and scratch is enough.
FROM scratch AS final

COPY --from=build /src/build/Release/ds-service /ds-service

EXPOSE 5051

ENTRYPOINT ["/ds-service"]
CMD ["--address", "0.0.0.0:5051"]
