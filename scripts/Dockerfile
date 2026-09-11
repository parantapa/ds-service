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
# so Conan compiles every C++ dependency from source:
# gRPC, protobuf, Abseil, OpenSSL, re2, zlib and c-ares.
# Expect the first build to take a long time.
#
# The `syntax` line is required,
# because the Conan profile uses a heredoc.

FROM alpine:3.24.1 AS build

# Alpine 3.24 ships gcc 15.2, cmake 4.2 and musl 1.2.6,
# which covers the C++23 and CMake >= 3.31 requirements.
#
# * The OpenSSL build that gRPC pulls in needs perl and linux-headers.
# * Any Conan dependency without a musllinux wheel needs python3-dev.
# * Sourcing Conan's generated conanbuild.sh needs bash.
RUN apk add --no-cache \
        g++ musl-dev linux-headers make cmake git bash \
        perl python3 python3-dev py3-pip \
    && pip install --break-system-packages --no-cache-dir conan

ENV CONAN_HOME=/opt/conan_home

# compiler.version tracks the gcc Alpine ships.
#
# cmake is declared platform-provided
# so that Conan uses the apk installed above and never downloads its own.
# ConanCenter's cmake package repackages Kitware's glibc binaries,
# which cannot run on musl.
# This line is needed even though conanfile.py does not tool_require cmake.
# The abseil, c-ares, openssl, re2 and zlib recipes each tool_require
# `cmake/[>=3.16]` or later to build themselves.
# Without this line Conan satisfies those from ConanCenter.
# The dependency build then dies with "Could not find CMAKE_ROOT".
# Conan does not check the platform tool's real version.
# The version declared here only has to satisfy those ranges,
# and the tool itself only has to satisfy
# CMakeLists.txt's `cmake_minimum_required(VERSION 3.31)`.
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
cmake/4.2

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
# so that a change to the sources does not invalidate the dependency build.
COPY conanfile.py ./
RUN conan install . --build=missing

# These lines copy exactly the paths conanfile.py exports.
COPY CMakeLists.txt ./
COPY cpp/ ./cpp/
COPY misc/ ./misc/

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# The build applies -static here rather than through the profile's exelinkflags
# so that it only affects this executable.
# -static on every dependency's configure-time link checks breaks some recipes.
#
# musl takes the default stack size for a thread from PT_GNU_STACK,
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
#
# readelf writes to a file instead of piping straight into grep.
# In `if readelf ... | grep -q INTERP`
# the shell consults only grep's status.
# A readelf that fails,
# because the binary moved or the tool is missing,
# feeds grep an empty input.
# The condition then reads as "no interpreter found",
# and the check passes without a check of anything.
RUN readelf -l build/Release/ds-service > /tmp/ds-service-phdrs.txt \
    && if grep -q INTERP /tmp/ds-service-phdrs.txt; then \
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
