# ds-service: Yet Another Data Structure Server

![Futuristic banner image.](misc/banner-image.png "Futuristic banner image.")

`ds-service` is a small, in-memory data structure server that is accessible via [gRPC](https://grpc.io/).

`ds-service` runs a single server process
that holds shared state in memory
and lets many distributed clients and workers coordinate using it.

Presently, it provides six data structures:
- **A key-value store** -- a shared `string -> bytes` store
    for passing data between processes.
- **A task queue** -- a priority-based work queue
    that distributes tasks to workers and tracks their state.
- **A journal store** -- append-only, ordered logs of binary entries.
- **A time series store** -- append-only series of
    timestamped floating-point values.
- **Named mutexes** -- cooperative locks
    for coordinating exclusive resource access across workers.
- **Counters** -- named monotonic counters
    that hand out successive integers.

Each of these is a separate key space with its own set of RPCs.

## Architecture

- **Server** (`cpp/ds-service.cpp`) -- a C++23 gRPC service.
    All state lives in memory,
    with a separate lock guarding each top-level data structure.
    Operations on one structure are serialized,
    while operations on different structures may run concurrently.
    Each RPC touches a single structure,
    so no request ever holds more than one lock.
    State is **not** persisted;
    that is, when the server stops all data is lost.
- **Client** (`python/ds_service_client/`) -- a Python 3.12+ client library
    that wraps the generated gRPC stubs
    and translates gRPC status codes into Python exceptions
    (`KeyError`, `ValueError`, `TimeoutError`).
- **Interface** (`misc/ds-service.proto`) -- the protobuf/gRPC contract
    shared by both sides.

## Additional Information

| Document | What it covers |
| --- | --- |
| [Data structure reference](docs/data-structure-reference.md) | Every RPC, its arguments and error statuses, and the exact semantics of each data structure. |
| [How to build the server](docs/howto-build-the-server.md) | Requirements, the Conan + CMake build, installing, running, and the container build. |
| [How to use the Python client](docs/howto-use-the-python-client.md) | Installing, connecting, the gRPC-status-to-exception mapping, and usage examples. |
| [How to run the tests](docs/howto-run-the-tests.md) | The pytest integration suite, pointing it at the binary, and what the fixtures provide. |
| [Debian packaging](docs/debian-packaging.md) | Building the `.deb` and setting the package version. |
