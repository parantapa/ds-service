# How to build the ds-service server

The server is a single C++23 binary, `ds-service`.
Dependencies are managed with [Conan](https://conan.io/)
and the build is driven by CMake.

## Requirements

- `build-essential` (or another C++23 toolchain — `g++` >= 14), `cmake` (>= 4.0), `git`
- [Conan](https://conan.io/) 2.x on `PATH` (`pip install conan`) —
    all the C++ dependencies come from Conan.

The first Conan run has to build a fair amount from source
so expect it to take a while.
Subsequent builds reuse the Conan cache.

## Building

From the repository root:

```sh
conan install . --build=missing
. build/Release/generators/conanbuild.sh
cmake -S . -B build/Release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake
cmake --build build/Release --parallel
```

The steps are:

1. `conan install` resolves and builds the dependencies and writes the
    CMake toolchain and dependency files into `build/Release/generators/`.
2. Sourcing `conanbuild.sh` puts the Conan-provided build tools —
    `protoc` and `grpc_cpp_plugin`, which the build needs — on `PATH`.
3. `cmake -S . -B ...` configures the build against that toolchain.
4. `cmake --build ...` compiles it.

The binary lands at `build/Release/ds-service`.
For a debug build, pass `-s build_type=Debug` to `conan install` and use
`-DCMAKE_BUILD_TYPE=Debug` with a matching `build/Debug` directory.

### Generated code

`misc/ds-service.proto` is the source of truth for the wire format.
The C++ protobuf and gRPC stubs (`ds-service.pb.*`, `ds-service.grpc.pb.*`)
are generated **automatically during the build**, into the build tree —
there is no manual step and they are not committed.

The Python client stubs are the one generated artifact *not* covered by
the C++ build: they are produced by `scripts/gen_python_bindings.sh` and
committed to the repository, so they only need regenerating when the
proto changes.

## Installing

```sh
cmake --install build/Release --prefix /usr/local
```

This installs the `ds-service` binary under `<prefix>/bin`.

## Running

```sh
ds-service --address 0.0.0.0:5051
```

The default address is `127.0.0.1:5051`.
Run `ds-service --help` for the full argument list.

## Container build

A reproducible container build is defined in
`scripts/apptainer/ds-service.def`:

```sh
apptainer build ds-service.sif scripts/apptainer/ds-service.def
./ds-service.sif --address 0.0.0.0:5051
```

It is a two-stage build
— the first stage clones the repository and builds it as above,
the second keeps only the resulting binary.
The definition file checks out a pinned tag,
so edit its `git checkout` line to build a different version.

## Debian package

To build a `.deb` instead, see [debian-packaging.md](debian-packaging.md).
