# Developer notes

Notes for people working on `ds-service` itself.

## The user-facing docs are the source of truth

The documents indexed in the README describe how the service behaves.
Read the one covering an area before changing code in it.

When behavior changes, update the document that covers it.

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
| `tests/` | The pytest integration suite. `conftest.py` holds the fixtures. One `test_*.py` per data structure, plus client, lifecycle, gRPC-option, server-helper, shutdown and error-translation tests. |
| `scripts/gen_python_bindings.sh` | Regenerates the committed Python stubs. |
| `scripts/update-version.sh` | Sets every version string in the repository. |
| `scripts/Dockerfile` | The static musl build. |
| `scripts/pb-dev.sh` | The author's own out-of-tree build wrapper. Not required to build the project. |

## Building, running, and testing

The [how to build the server](howto-build-the-server.md) guide
covers how to build and run the server, including the static musl image.

The suite in `tests/` is an integration suite driven by
[pytest](https://pytest.org/).
Almost every test starts a real `ds-service` process
and drives it through the Python client over gRPC.
There are no unit tests of the C++ in isolation.
The exceptions run in process and need no binary:
`test_client_parity.py`, `test_translate_grpc_error.py`,
and the resolver tests in `test_server_helper.py`.
So the tests need a built server,
and the test dependencies installed:

```sh
pip install -e ".[test]"
```

That pulls in `pytest`.
You do not have to install the package to use the client itself.
`pyproject.toml` sets `pythonpath = ["python"]` for pytest,
so the suite imports `ds_service_client` straight from the source tree.

### Pointing the tests at the binary

The fixtures start the server through
`ds_service_client`'s own `DsServiceServer` helper,
which locates it in one of two ways, in order:

1. `DS_SERVICE_BIN`, if set. It can be a whole command,
    such as `docker run --rm --network host ds-service`, not only a path.
2. Otherwise, a `ds-service` found on `PATH`.

After an in-tree build, point the variable at the binary:

```sh
export DS_SERVICE_BIN=build/Release/ds-service
```

If neither is available,
every test that needs the binary fails with a `FileNotFoundError`.

### Running the suite

```sh
python -m pytest                 # everything
python -m pytest tests/test_journal.py
python -m pytest tests/test_tasks.py::test_add_get_done_lifecycle
```

`testpaths = ["tests"]` in `pyproject.toml`
means a bare `python -m pytest` finds the suite in the repository root.

After a change to `misc/ds-service.proto` or the C++ server,
rebuild the binary before you run the suite.
After a proto change, also run `scripts/gen_python_bindings.sh`.
Without these steps, the suite exercises stale code.

For the fixtures themselves, see [the test harness](#the-test-harness).

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

| Library | Used for |
| --- | --- |
| `grpcio` and `protobuf` | The generated stubs. |
| `ifaddr` | Resolving an interface name to an address in `server.py`. |
| `pytest` | The suite. |
| `grpcio-tools` | `scripts/gen_python_bindings.sh`. |

Check the C++ with clangd and the Python with pyright.
Format the C++ with clang-format and the Python with black.

## Generated code

`misc/ds-service.proto` is the source of truth for the wire format.
Two generators consume it, and they behave differently.

The build generates the C++ protobuf and gRPC stubs
(`ds-service.pb.*`, `ds-service.grpc.pb.*`)
into the build tree.
There is no manual step and they are not committed.

The C++ build does not cover the Python stubs
(`ds_service_pb2.py`, `ds_service_pb2.pyi`, `ds_service_pb2_grpc.py`).
`scripts/gen_python_bindings.sh` produces them,
and the repository keeps them in version control,
so you regenerate them only when the proto changes.

Two rules follow.

### Never edit a generated file directly

Two of the three Python stubs carry a "DO NOT EDIT" banner,
and the repository holds them anyway.
That makes them easy to edit by mistake,
and the next regeneration overwrites the edit without a warning.

### A proto change is a four-step job

Only step 2 generates code on its own.

1. Edit `misc/ds-service.proto`.
2. Rebuild the C++,
    which regenerates `ds-service.pb.*` and `ds-service.grpc.pb.*`.
3. Run `scripts/gen_python_bindings.sh`.
    This one is manual. If you skip it, the Python client goes stale.
4. Hand-update `cpp/ds-service.cpp`
    and `python/ds_service_client/client.py`
    to implement and expose the change.
    A new RPC means a method on both clients in `client.py`.
    See "Two clients, one API".

### Regeneration moves the client's dependency floors

Step 3 stamps the toolchain's own version into the committed stubs.
Both stubs then refuse to import against an older runtime:

- `ds_service_pb2_grpc.py` raises
    when `grpcio` is below its `GRPC_GENERATED_VERSION`.
- `ds_service_pb2.py` raises
    when the `protobuf` runtime is older than the gencode it came from.

After step 3, re-derive the `dependencies` floors in `pyproject.toml`
from the regenerated stubs.
Otherwise the package resolves to a runtime that cannot import it.

If you regenerate with a newer `grpcio-tools`
than the one that produced the committed stubs,
the client's minimum requirements rise for everybody.
Unless you intend to raise those floors,
pin `grpcio-tools` to the version already recorded in the stubs.

## Two clients, one API

`client.py` holds two hand-written clients,
`DsServiceClient` over grpc's blocking channel
and `DsServiceClientAsync` over `grpc.aio`.
They are separate classes on purpose.
The two channels are different objects,
and one class that returns either `bytes` or an awaitable
defeats both the reader and pyright.
That leaves every RPC written out twice.

`tests/test_client_parity.py` is what keeps the copies in step.
It fails in four cases:

- The two classes stop offering the same method names.
- A shared method's parameters or return annotation drift apart.
- An async method is not a coroutine function.
- A class loses its own context manager protocol.

Add an RPC to one client without the other and it says so.

`client.py` keeps the shared parts at module level rather than copying them:
`translate_grpc_error`, `GRPC_CLIENT_OPTIONS`,
the timeout and mutex constants,
and the `as_queue_list`, `time_series_get_request` and `mutex_retry_delay` helpers.
Only the stub call and its `await`
must differ between the two copies of a method.
`mutex_acquire` is the exception, because its retry sleep differs as well.

## The channel settings are one setting in two languages

Two pieces of configuration are only correct as a matched pair
between `cpp/ds-service.cpp` and `python/ds_service_client/client.py`,
and neither language can check the other.
`tests/test_grpc_options.py` is what keeps them in step.
Change either side and run it.

**The maximum message size.**
`MAX_MESSAGE_SIZE_BYTES` exists in both files and must hold the same value.
If the two disagree, one side rejects what the other sends,
and the sender sees a `RESOURCE_EXHAUSTED` error
with no cause in its own code.

**The keepalive settings.**
The client's ping interval must stay above the server's
`GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS`.
Below that interval, the server answers pings with GOAWAY/ENHANCE_YOUR_CALM
and drops the connection,
which callers see as a `TimeoutError` with no mention of pings.
If you raise the ping rate on the client,
lower that interval on the server in the same change.

## The server refuses to share its port

gRPC enables `SO_REUSEPORT` by default,
so a second `ds-service` started on an address that is already bound
joins the first rather than failing.
State is in memory, and the processes do not share it.
The result is two servers with divergent state.
Clients split between them, and nothing indicates it.

`cpp/ds-service.cpp` therefore sets `GRPC_ARG_ALLOW_REUSEPORT` to `0`,
and a second server on a bound address exits with a bind failure.

`DsServiceServer` enforces the same rule from the client side.
`_check_port_free` in `python/ds_service_client/server.py`
probes an explicit port before the constructor starts the process.
The caller then gets an `OSError` from the constructor
rather than a helper object that addresses somebody else's server.
That probe sets `SO_REUSEADDR` because gRPC's own listener sets it.
Without it, the probe also refuses a port left in `TIME_WAIT`
by a server that already exited.
The real server binds such a port without complaint.

Two tests hold this in place:
`test_second_server_on_the_same_port_fails` in `tests/test_grpc_options.py`,
and `test_explicit_port_already_in_use_is_refused`
in `tests/test_server_helper.py`.

## The test harness

The fixtures live in `tests/conftest.py`:

| Fixture | Yields |
| --- | --- |
| `server_binary` | How to start the server under test: a path, or a whole command line. |
| `loopback_interface` | The name of the interface holding `127.0.0.1`, which is what test servers bind. |
| `server_process` | `(proc, address)` for a running server. For tests that drive the process itself, such as signaling it. |
| `server` | The address of a running server. |
| `client` | A connected `DsServiceClient`, closed at the end of the test. |

Each test gets a fresh server process on its own free port.
That isolates the server's in-memory state between tests,
and it lets the suite run without a fixed port.
Every one of them binds the loopback interface,
so a test run is never reachable from another machine.

`DsServiceServer` starts and stops the process, not the harness,
so the fixtures cannot drift from the helper the client library ships.
Startup waits for the port to accept a TCP connection.
Startup then makes one read-only RPC,
which confirms that the service is registered and answers.
Teardown terminates the process.
If the process does not exit within the grace period `DsServiceServer` allows,
teardown kills it
(`TERMINATE_TIMEOUT_S` in `python/ds_service_client/server.py`).

## Conventions

- After changing C++, check it with clangd.
    Pass `--compile-commands-dir` explicitly.
    Do not update the top-level `compile_commands.json` symlink.
- `.clang-format` in the repository root sets the C++ formatting.
- After changing Python, check it with pyright and format it with black.
    `pyproject.toml` configures both.
    It excludes the generated stubs from pyright,
    and black does not format them.
- Use semantic line breaks in documentation, block comments,
    and docstrings.

## Versioning

`scripts/update-version.sh <version>`
sets every version string in the repository:
`cpp/ds-service.cpp`, `CMakeLists.txt`, `pyproject.toml`,
and `conanfile.py`.
Set them through the script rather than by hand.
`CMakeLists.txt` receives the leading numeric part only,
because `project(VERSION)` rejects a pre-release suffix.
The script greps for each line before touching anything,
so it edits nothing unless all of them are present.

## Known limitations

The server never reclaims `TaskTable` rows.
A task keeps its row for the life of the server process,
even after it reaches `Complete` or `Canceled`.
A long-lived server therefore holds one row per task ever added,
rather than one per task still outstanding.
`TaskSearchId` walks that table,
so its cost grows the same way.

Compaction is not a local change.
The index of a row is its address,
and `TaskManager::task_index` and every queue entry in `TaskManager::queue`
hold that index.
If you move a row, you rewrite both.

The server does not reclaim dead queue entries either.
A heap does not allow an erase from the middle.
`TaskGet` therefore discards dead entries only as it pops them,
and it never pops an entry that sorts below the live work.
`TaskSetPriority` is the way to accumulate them.
A rise in a task's priority leaves an entry at the old, lower value,
and a busy queue never reaches that entry.
A client that reprioritizes on a loop therefore grows the heap
by one entry per call per queue.

The user-facing consequences of both are in
[about the task queue](about-the-task-queue.md).
