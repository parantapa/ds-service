# How to build the ds-service server

The server is a single C++23 binary, `ds-service`.
[Conan](https://conan.io/) manages the dependencies.
CMake drives the build.

## Requirements

- `build-essential` (or another C++23 toolchain, `g++` 14 or later), `cmake` (>= 3.31), `git`
- [Conan](https://conan.io/) 2.x on `PATH` (`pip install conan`).
    All the C++ dependencies come from Conan.

The first Conan run builds the missing dependencies from source,
and takes far longer than a later build.
Subsequent builds reuse the Conan cache.

## Build the server

From the repository root:

```sh
conan install . --build=missing
. build/Release/generators/conanbuild.sh
cmake -S . -B build/Release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake
cmake --build build/Release --parallel
```

Source `conanbuild.sh` in the same shell as the `cmake` calls.
It puts the Conan-provided `protoc` and `grpc_cpp_plugin` on `PATH`.
The build fails to configure without them.

The build writes the binary to `build/Release/ds-service`.
For a debug build,
pass `-s build_type=Debug` to `conan install`.
Then use `-DCMAKE_BUILD_TYPE=Debug` with a matching `build/Debug` directory.

### After changing the proto

The build regenerates the C++ stubs.
Run the build commands again.
The C++ build does not cover the Python client stubs.
Run `scripts/gen_python_bindings.sh` from the repository root as well.
It needs `grpcio-tools` installed.
See the [developer notes](developer-notes.md#generated-code)
for the full inventory of generated files.

## Install the binary

```sh
cmake --install build/Release --prefix /path/to/prefix
```

This installs the `ds-service` binary under `/path/to/prefix/bin`.

## Run the server

```sh
ds-service --address 0.0.0.0:5051
```

The default address is `127.0.0.1:5051`.
Run `ds-service --help` for the full argument list.

## Static musl binary

`scripts/Dockerfile` builds `ds-service` against musl on Alpine 3.24,
and links it statically:

```sh
docker build -f scripts/Dockerfile -t ds-service:static .
docker run --rm -p 5051:5051 ds-service:static
```

The final stage is `FROM scratch`,
so the image holds nothing but the binary.
To extract that binary instead of running it:

```sh
docker build -f scripts/Dockerfile --output type=local,dest=./dist .
```

ConanCenter has no musl binaries.
The first build compiles the whole dependency tree from source.
It takes longer than the normal build.

The Conan profile and linker flags used here differ from the normal build.
See [about the static musl build](about-the-static-musl-build.md)
for the differences, and for the reason the build needs them.
