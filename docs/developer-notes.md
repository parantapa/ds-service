# Developer Notes

Notes for people working on `ds-service` itself.

## The user-facing docs are the source of truth

The documents indexed in the README describe how the service behaves.
Read the one covering an area before changing code in it.

When behaviour changes, update the document that covers it.

## Map of the source

| Path | Contents |
| --- | --- |
| `misc/ds-service.proto` | The protobuf/gRPC contract. The authoritative definition of the wire format, and the input to every code generator here. |
| `cpp/ds-service.cpp` | The entire server: state structs, the `DsServiceImpl` service, signal handling, and `main`. There is no other C++ source file. |
| `CMakeLists.txt` | Builds the generated stubs into `ds-service-grpc`, then the `ds-service` executable. |
| `conanfile.py` | The C++ dependency set and the build/tool requirements. |
| `python/ds_service_client/__init__.py` | The package's public surface: the two clients, `DsServiceServer`, the exceptions, and `TaskState`. |
| `python/ds_service_client/client.py` | Both hand-written clients, the exception translation, and the shared gRPC options and helpers. |
| `python/ds_service_client/server.py` | `DsServiceServer`, which runs a `ds-service` process for the life of the object. |
| `python/ds_service_client/ds_service_pb2*.py`, `*.pyi` | Generated Python stubs, committed. Never edited by hand. |
| `python/ds_service_client/ds-service.proto` | A copy of `misc/ds-service.proto`, placed there by the generator script. Not the source of truth. |
| `tests/` | The pytest integration suite. `conftest.py` holds the fixtures; one `test_*.py` per data structure, plus client, lifecycle and gRPC-option tests. |
| `scripts/gen_python_bindings.sh` | Regenerates the committed Python stubs. |
| `scripts/update-version.sh` | Sets every version string in the repository. |
| `scripts/Dockerfile` | The static musl build. |
| `scripts/pb-dev.sh` | The author's own out-of-tree build wrapper. Not required to build the project. |

## Building, running, and testing

Building and running the server, including the static musl image,
is covered in [how to build the server](howto-build-the-server.md).

Running the suite is covered in
[how to run the tests](howto-run-the-tests.md).
Every test starts a real server process and drives it over gRPC;
there are no unit tests of the C++ in isolation.

## Tools, libraries, and frameworks

Server, all resolved through Conan 2.x and built with CMake (>= 3.31)
against a C++23 toolchain:

| Library | Used for |
| --- | --- |
| gRPC and protobuf | The service itself and the generated stubs. |
| `re2` | The regular expressions behind every `SearchKey` RPC. |
| `parallel-hashmap` (`phmap`) | The maps holding the server's state. |
| `spdlog` | Logging. |
| `argparse` | Command-line parsing in `main`. |

Client, on Python 3.12+:
`grpcio` and `protobuf` for the generated stubs,
`ifaddr` for resolving an interface name to an address in `server.py`,
`pytest` for the suite,
`grpcio-tools` for `scripts/gen_python_bindings.sh`.

Checked with clangd and pyright; formatted with clang-format and black.

## Generated code

`misc/ds-service.proto` is the source of truth for the wire format.
Two generators consume it, and they behave differently.

The C++ protobuf and gRPC stubs
(`ds-service.pb.*`, `ds-service.grpc.pb.*`)
are generated **automatically during the build**, into the build tree.
There is no manual step and they are not committed.

The Python stubs
(`ds_service_pb2.py`, `ds_service_pb2.pyi`, `ds_service_pb2_grpc.py`)
are **not** covered by the C++ build.
They are produced by `scripts/gen_python_bindings.sh`
and committed to the repository,
so they only need regenerating when the proto changes.

Two rules follow.

**Never edit a generated file directly.**
The Python stubs carry a "DO NOT EDIT" banner and are committed anyway,
which makes them easy to edit by mistake
and easy to have an edit silently overwritten.

**A proto change is a four-step job,**
and only the first two happen on their own:

1. Edit `misc/ds-service.proto`.
2. Rebuild the C++,
    which regenerates `ds-service.pb.*` and `ds-service.grpc.pb.*`.
3. Run `scripts/gen_python_bindings.sh`.
    This one is **manual**;
    skip it and the Python client silently goes stale.
4. Hand-update `cpp/ds-service.cpp` and
    `python/ds_service_client/client.py`
    to implement and expose the change.
    A new RPC means a method on **both** clients in `client.py`;
    see "Two clients, one API" below.

### Regenerating moves the client's dependency floors

Step 3 stamps the toolchain's own version into the committed stubs,
and both stubs refuse to import against an older runtime:
`ds_service_pb2_grpc.py` raises when `grpcio` is below its
`GRPC_GENERATED_VERSION`,
and `ds_service_pb2.py` raises when the `protobuf` runtime
is older than the gencode it was built from.
The `dependencies` floors in `pyproject.toml` have to be re-derived
from the regenerated stubs whenever step 3 runs,
or the package resolves to a runtime that cannot import it.

Regenerating with a newer `grpcio-tools`
than the one that produced the committed stubs
therefore raises the client's minimum requirements for everybody.
Pin `grpcio-tools` to the version already recorded in the stubs
unless raising those floors is the actual intent.

## Two clients, one API

`client.py` holds two hand-written clients,
`DsServiceClient` over grpc's blocking channel
and `DsServiceClientAsync` over `grpc.aio`.
They are separate classes on purpose --
the two channels are different objects,
and one class returning either `bytes` or an awaitable
would defeat both the reader and pyright --
which leaves every RPC written out twice.

`tests/test_client_parity.py` is what keeps the copies in step.
It fails when the two classes stop offering the same method names,
when a shared method's parameters or return annotation drift apart,
and when an async method is not a coroutine function.
Add an RPC to one client without the other and it says so.

What is genuinely shared is shared at module level rather than copied:
`translate_grpc_error`, `GRPC_CLIENT_OPTIONS`,
the timeout and mutex constants,
and the `as_queue_list`, `time_series_get_request` and `mutex_retry_delay` helpers.
Only the stub call and its `await`
should differ between the two copies of a method.

## The channel settings are one setting in two languages

Two pieces of configuration are only correct as a matched pair
between `cpp/ds-service.cpp` and `python/ds_service_client/client.py`,
and neither language can check the other.
`tests/test_grpc_options.py` is what keeps them in step;
change either side and run it.

**The maximum message size.**
`MAX_MESSAGE_SIZE_BYTES` exists in both files and must hold the same value.
If the two disagree, one side rejects what the other happily sends,
and the sender sees a `RESOURCE_EXHAUSTED` it did nothing to earn.

**The keepalive settings.**
The client's ping interval must stay above the server's
`GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS`.
Below it, the server answers pings with GOAWAY/ENHANCE_YOUR_CALM
and drops the connection,
which callers see as a `TimeoutError` with no mention of pings.
Raising the ping rate on the client therefore means
lowering that interval on the server in the same change.

## The test harness

The fixtures live in `tests/conftest.py`:

| Fixture | Yields |
| --- | --- |
| `server_binary` | How to start the server under test -- a path, or a whole command line. |
| `loopback_interface` | The name of the interface holding `127.0.0.1`, which is what test servers bind. |
| `server_process` | `(proc, address)` for a running server -- for tests that drive the process itself, such as signalling it. |
| `server` | The address of a running server. |
| `client` | A connected `DsServiceClient`, closed at the end of the test. |

Each test gets a **fresh server process on its own free port**,
so the server's in-memory state is isolated between tests
and the suite can run without a fixed port.
Every one of them binds the loopback interface,
so a test run is never reachable from another machine.

Starting and stopping the process is `DsServiceServer`'s job,
not the harness's,
so the fixtures cannot drift from the helper the client library ships.
Startup waits for the port to accept a TCP connection
and then makes one read-only RPC,
which confirms the service is registered and answering;
teardown terminates the process,
escalating to a kill
if it does not exit within the grace period `DsServiceServer` allows
(`TERMINATE_TIMEOUT_S` in `python/ds_service_client/server.py`).

## Conventions

- After changing C++, check it with clangd.
    Pass `--compile-commands-dir` explicitly;
    do not update the top-level `compile_commands.json` symlink.
- C++ formatting is enforced by `.clang-format` in the repository root.
- After changing Python, check it with pyright and format it with black.
    Both are configured in `pyproject.toml`;
    the generated stubs are excluded from pyright
    and are not black-formatted.
- Use semantic line breaks in documentation, block comments,
    and docstrings.

## Versioning

Every version string in the repository is set by
`scripts/update-version.sh <version>`:
`cpp/ds-service.cpp`, `CMakeLists.txt`, `pyproject.toml`,
and `conanfile.py`.
Set them through the script rather than by hand.
`CMakeLists.txt` receives the leading numeric part only,
because `project(VERSION)` rejects a pre-release suffix.
The script greps for each line before touching anything,
so it edits nothing unless all of them are present.

## Known limitations

`TaskTable` rows are never reclaimed.
A task keeps its row for the life of the server process,
including after it reaches `Complete` or `Canceled`,
so a long-lived server accumulates rows
in proportion to the total number of tasks ever added
rather than the number currently outstanding.
`TaskSearchId` walks that table,
so its cost grows the same way.

Compaction is not a local change:
a row is addressed by its index,
and that index is held by `TaskManager::task_index`
and by every queue entry in `TaskManager::queue`,
so moving a row means rewriting both.

Dead queue entries are not reclaimed either.
Nothing can be erased from the middle of a heap,
so `TaskGet` discards dead entries only as it pops them,
and an entry that sorts below the live work is never popped.
`TaskSetPriority` is the way to accumulate them:
raising a task's priority leaves an entry at the old, lower value
that a busy queue never reaches,
so a client that re-prioritizes on a loop grows the heap
by one entry per call per queue.

The user-facing consequences of both are in
[about the task queue](about-the-task-queue.md).
