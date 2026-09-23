# Developer notes

Notes for people working on `ds-service` itself.

## Map of the source

| Path | Contents |
| --- | --- |
| `cpp/common/include/ds-service/` | The plain C++ types that define the operations, shared by the server and the C++ client: one struct per request and response in `messages.hpp`, whose comments state the contract of each operation, `TaskState`, and `ErrorCode`, `Result` and `ClientError` in `error.hpp`. |
| `cpp/server/core/` | The server's data structures. `data-structures.hpp` declares one struct per top level data structure, with the lock that guards it and one method per operation. One `.cpp` file implements each structure. `system-state.hpp` holds them all. |
| `cpp/server/transport.hpp` | `ServerTransport`, the interface every server transport implements. |
| `cpp/server/main.cpp` | `main`, the entry point: argument parsing, signal handling, and the start and shutdown of every transport. |
| `cpp/client/` | The C++ client. `ds::Client` and the `ClientTransport` interface in `client.hpp` and `client-transport.hpp`, and `ds::connect()` in `connect.hpp`, which picks a transport from an address. |
| `cpp/grpc/` | Everything gRPC: the proto, the channel settings, the codec between plain types and protobuf messages, the server transport in `server/`, and the client transport in `client/`. |
| `cpp/grpc/ds-service.proto` | The gRPC encoding of the plain types, and so the definition of the gRPC wire format. |
| `cpp/python/bindings.cpp` | The nanobind module `ds_service_client._ext`, which binds `ds::Client` and the message types it returns. |
| `cpp/tests/client-smoke.cpp` | A smoke test of the C++ client against a real server, run by ctest. |
| `CMakeLists.txt` | Every C++ target, and the options that select them. See [the transport boundary](#the-transport-boundary). |
| `cmake/conan-install.cmake` | Runs `conan install` at configure time, for a build that starts from pip. |
| `conanfile.py` | The C++ dependency set, the build/tool requirements, and the `with_*` options that select what the build produces. |
| `pyproject.toml` | The Python package, built with scikit-build-core, and the configuration for pytest, pyright, black and cibuildwheel. |
| `python/ds_service_client/__init__.py` | The package's public surface: what `ds_service_client` re-exports. |
| `python/ds_service_client/client.py` | Both Python clients, over `_ext`, the error translation, and the shared helpers. |
| `python/ds_service_client/errors.py` | The exceptions the clients raise, beyond the built-in ones. |
| `python/ds_service_client/server.py` | `DsServiceServer`, which starts a `ds-service` process and stops it on `close()` or at the end of a `with` block. |
| `docs/` | The user documentation, and these notes. |
| `misc/` | The banner image of the README. |
| `tests/` | The pytest integration suite. `conftest.py` holds the fixtures. One `test_*.py` per data structure, plus client, lifecycle, extension module, fork, no-gRPC-import, server-helper, shutdown and error-translation tests. `tests/grpc_transport/` holds the tests that only make sense over gRPC. |
| `scripts/update-version.sh` | Sets every version string in the repository. |
| `scripts/Dockerfile` | The static musl build. |
| `scripts/pb-dev.sh` | The author's own out-of-tree build wrapper. Not required to build the project. |

## Building, running, and testing

The [how to build the server](how-to-guides/build-the-server.md) guide
covers how to build and run the server,
how to build the Python module,
and the static musl image.

There are two suites.
`ctest` runs `cpp/tests/client-smoke.cpp`,
which starts a server and drives it through the C++ client.
The suite in `tests/` is an integration suite driven by
[pytest](https://pytest.org/).
Most of its tests start a real `ds-service` process
and drive it through the Python client.
There are no unit tests of the C++ in isolation.

The pytest suite imports `ds_service_client` straight from the source tree,
because `pyproject.toml` sets `pythonpath = ["python"]`.
The package there needs the extension module `_ext`.
A build with `-DDS_SERVICE_BUILD_PYTHON=ON` compiles it,
and copies it and its type stub into `python/ds_service_client/`.
`.gitignore` covers both copies.
So the suite needs a server binary, a build with the Python module,
and the development and test dependencies:

```sh
pip install black build cibuildwheel conan ifaddr nanobind pyright pytest scikit-build-core twine
```

The list is the `dev` and `test` extras in `pyproject.toml`,
plus `ifaddr`, the one run-time dependency.
`pip install -e ".[dev,test]"` installs them as well,
but it also builds the whole package, which the suite does not need.

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
ctest --test-dir build/Release   # the C++ smoke test
python -m pytest                 # everything in tests/
python -m pytest tests/test_journal.py
python -m pytest tests/test_tasks.py::test_add_get_done_lifecycle
```

`testpaths = ["tests"]` in `pyproject.toml`
means a bare `python -m pytest` finds the suite in the repository root.

After a change to any C++, rebuild before you run either suite.
The build rebuilds both the server binary and the Python module.
Without it, the suite exercises stale code.

For the fixtures themselves, see [the test harness](#the-test-harness).

## Tools, libraries, and frameworks

C++, all resolved through Conan 2.x and built with CMake (>= 3.31)
against a C++23 toolchain:

| Library | Used for |
| --- | --- |
| gRPC and protobuf | The gRPC transports on both sides, and the code generated from the proto. |
| `re2` | The regular expressions behind `task_search_id` and every `*_search_key` operation. Server only. |
| `parallel-hashmap` (`phmap`) | The maps holding the server's state. Server only. |
| `spdlog` | Logging. Server only. |
| `argparse` | Command-line parsing in `main`. Server only. |

Python, on 3.12+:

| Library | Used for |
| --- | --- |
| `nanobind` | The extension module `_ext`. Needed to build it, not to run it. |
| `scikit-build-core` | The build backend that builds the wheel through CMake. |
| `ifaddr` | Resolving an interface name to an address in `server.py`. The one run-time dependency. |
| `pytest` | The suite. |
| `cibuildwheel` | The manylinux wheel. It needs docker or podman. |

The formatters and checkers are under [Conventions](#conventions).

## The plain types define the system

The operations of `ds-service` are defined in C++,
apart from any transport.
`cpp/common/include/ds-service/messages.hpp` holds one struct per request and response.
The comment on each request struct states the contract of its operation:
what it does, and what it refuses and with which `ErrorCode`.
The comment on a response struct states what its fields report.
`task_get_count_by_state` takes no request,
so the comment on its response struct states its contract.
`error.hpp` holds `ErrorCode`, and `task-state.hpp` holds `TaskState`.
The server core in `cpp/server/core/` implements those contracts.
Outside `cpp/grpc/`, documentation and comments name an operation
by the snake_case name both clients use, such as `task_get`,
and an error by its `ErrorCode`, such as `NotFound`.

A transport only carries these types between a client and the server.
gRPC is the one transport today.
Its encoding is `cpp/grpc/ds-service.proto`,
whose messages have the same names and field names as the plain structs,
and `cpp/grpc/codec.cpp` converts between the two.
Each RPC there is named after its operation in CamelCase,
such as `TaskGet` for `task_get`.

## Generated code

The build generates the C++ protobuf and gRPC stubs
(`ds-service.pb.*`, `ds-service.grpc.pb.*`) from the proto
into the build tree.
The build also generates `_ext.pyi`, the type stub of the Python module.
Nothing generated is committed,
and there is no manual generation step.
Only the code under `cpp/grpc/` uses the generated types.

## A new operation touches every layer

A new or changed operation is a change to each of these, in order:

1. The plain structs in `cpp/common/include/ds-service/messages.hpp`,
    with the contract of the operation in the comment on its request struct.
2. The server core: the method on the data structure
    in `cpp/server/core/data-structures.hpp`,
    defined in that structure's own `.cpp` file.
3. The C++ client: the method on `ClientTransport`
    and the method on `ds::Client`,
    which `cpp/client/client.cpp` defines.
4. The encoding of the operation in each transport.
    For gRPC, that is four places:
    - The RPC and its messages in `cpp/grpc/ds-service.proto`,
        with the same names and field names as the plain structs.
    - The codec in `cpp/grpc/codec.hpp` and `cpp/grpc/codec.cpp`,
        in both directions.
    - The `DsServiceImpl` method
        in `cpp/grpc/server/grpc-server-transport.cpp`
        that calls the server core.
    - The override of the `ClientTransport` method
        in `cpp/grpc/client/grpc-client-transport.cpp`.
5. The binding in `cpp/python/bindings.cpp`.
6. The method on both Python clients in `client.py`.
    See "Two clients, one API".

Some drift between the layers fails the build.
A transport that does not override every `ClientTransport` method
fails where it is constructed.
The `static_assert`s on `TaskState` in `cpp/grpc/codec.cpp` fail
when the plain enum and the proto number a value differently,
or when the proto gains a value.
`tests/test_ext.py` compares the Python enum with the proto.

## The transport boundary

All gRPC code lives under `cpp/grpc/`.
The rest of the C++ knows nothing of any transport:
the server core sees plain requests,
and `ds::Client` sees a `ClientTransport`.
A second transport is a new sibling directory of `cpp/grpc/`,
with a `ServerTransport` for `main.cpp` to start
and a `ClientTransport` for `ds::connect()` to return.

The build enforces the boundary.
`ds-service-common`, `ds-service-core` and `ds-service-client`
never link gRPC or protobuf,
so a gRPC include in any of them fails to compile.
`ds::connect()` has to construct every transport,
so it lives in its own target, `ds-service-connect`,
which the Python module and C++ programs link.

The Python package is on the far side of the same boundary.
It imports neither `grpc` nor `google.protobuf`,
and `tests/test_no_grpc_import.py` fails if it starts to.

The server core returns `ds::Result<T>`, an alias of `std::expected`,
because a refusal there is an ordinary outcome that a transport turns into a wire status.
The C++ client throws `ds::ClientError` instead.
Both carry an `ErrorCode`.
`cpp/grpc/codec.hpp` maps each code to a gRPC status and back,
and `static_assert`s in `cpp/grpc/codec.cpp` check the round trip
of every code a transport carries.

## Two clients, one API

`client.py` holds two Python clients,
`DsServiceClient`, which blocks,
and `DsServiceClientAsync`, whose methods are coroutines.
Both call the same C++ client in `_ext`.
The async client runs each call on a thread of its own pool,
through `loop.run_in_executor`,
which works because every call in `_ext` releases the GIL.
They are separate classes on purpose.
One class that returns either `bytes` or an awaitable
defeats both the reader and pyright.
That leaves every method written out twice.

`tests/test_client_parity.py` is what keeps the copies in step.
It fails in four cases:

- The two classes stop offering the same method names.
- A shared method's parameters or return annotation drift apart.
- An async method is not a coroutine function.
- The async class loses its async context manager protocol.

Add a method to one client without the other and it says so.

`client.py` keeps the shared parts at module level rather than copying them:
`translate_error`, `connect`,
the timeout and mutex constants,
and the `as_queue_list`, `as_parent_task_id_list` and `mutex_retry_delay` helpers.
Only the call itself differs between the two copies of a method:
`self.client.map_get(key)` in one,
and `await self._call(self.client.map_get, key)` in the other.
`mutex_acquire` is one exception, because its retry sleep differs as well.
`close` is the other, because the async client also shuts down its pool.

## The channel settings live in one header

Two pieces of the gRPC transport's configuration
are only correct as a matched pair
between the server and its clients.
Both sides come from `cpp/grpc/channel-settings.hpp`.
One constant there serves both sides for the message size,
and a `static_assert` there checks the keepalive pair at compile time.

**The maximum message size.**
`MAX_MESSAGE_SIZE_BYTES` applies to both sides.
If the two ever disagree, one side rejects what the other sends,
and the sender sees a message-too-large error
with no cause in its own code.

**The keepalive settings.**
`CLIENT_KEEPALIVE_TIME_MS` must stay above
`SERVER_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS`.
Below that interval, the server answers pings with GOAWAY/ENHANCE_YOUR_CALM
and drops the connection,
which callers see as a `TimeoutError` with no mention of pings.
A client built from an older release still has its own copy of these values,
so lower the server's interval before you raise the client's ping rate.

## A client does not survive fork()

This is a limit of the gRPC transport.
The gRPC inside `_ext` does not survive `fork()`.
Once a process has created a client,
a call from a child it forks hangs,
on an inherited client and on a new one alike.
A child forked before the process created any client works.
The 6.x client, on `grpcio`, did not have this problem,
because `grpcio` registers gRPC's own fork handlers.

Setting the `GRPC_ENABLE_FORK_SUPPORT` environment variable did not help.
gRPC core registers its fork handlers with `pthread_atfork`
only when it is compiled with `GRPC_POSIX_FORK_ALLOW_PTHREAD_ATFORK`,
and its posix event engine only with `GRPC_ENABLE_FORK_SUPPORT` as well
(`src/core/lib/iomgr/fork_posix.cc`, `src/core/lib/event_engine/posix_engine/posix_engine.cc`).
The `setup.py` that builds `grpcio` defines both,
as do the Ruby extension build and the `fork_support` config in `tools/bazel.rc`.
The CMake build of gRPC, which Conan uses, defines neither.
Two ways out were not tried:
building gRPC through Conan with both macros defined,
and calling gRPC's internal prefork and postfork hooks from `_ext`,
which would tie the module to gRPC internals.

So `client.py` refuses instead of hanging.
`connect()` records the first client,
an `os.register_at_fork` hook marks any child forked after it,
and `translate_error` raises `RuntimeError(FORK_MESSAGE)` there
before a call reaches `_ext`.
`tests/test_fork.py` covers both sides of the rule.
The cost is that such a child cannot use the client at all.
`multiprocessing` callers use the `spawn` or `forkserver` start method.
The C++ client has no such guard, and a C++ child hangs.

## Pip builds run conan install from CMake

pip runs CMake through scikit-build-core with no Conan toolchain.
So `pyproject.toml` sets `DS_SERVICE_CONAN_INSTALL`,
and `cmake/conan-install.cmake` runs `conan install` before `project()`,
then builds with the toolchain it generated.
Every other build passes the toolchain on the command line,
and the file does nothing.

The alternative, the `cmake-conan` dependency provider,
would mean vendoring a large third-party CMake file.
The cost is the build time with no Conan cache:
gRPC, protobuf and their dependencies compile from source,
which took about five and a half minutes on 16 cores.
A wheel build in a fresh manylinux container pays that every time,
unless the CI caches `CONAN_HOME`.

Because of this file, `cmake/` is part of every build.
`exports_sources` in `conanfile.py` and the `COPY` lines in `scripts/Dockerfile`
carry it.

## The server refuses to share its port

This is a behavior of the gRPC transport.
gRPC enables `SO_REUSEPORT` by default,
so a second `ds-service` started on an address that is already bound
joins the first rather than failing.
State is in memory, and the processes do not share it.
The result is two servers with divergent state.
Clients split between them, and nothing indicates it.

`cpp/grpc/server/grpc-server-transport.cpp` therefore sets `GRPC_ARG_ALLOW_REUSEPORT` to `0`,
and a second server on a bound address exits with a bind failure.

`DsServiceServer` enforces the same rule from the client side.
`_check_port_free` in `python/ds_service_client/server.py`
probes an explicit port before the constructor starts the process.
The caller then gets an `OSError` from the constructor
rather than a helper object that addresses somebody else's server.
The probe's socket options are explained at the probe.

Two tests hold this in place:
`test_second_server_on_the_same_port_fails`
in `tests/grpc_transport/test_grpc_options.py`,
and `test_failed_start_leaves_the_running_server_alone`
in `tests/test_server_helper.py`.

## The test harness

The fixtures live in `tests/conftest.py`,
and the docstring of each states what it yields.

Each test gets a fresh server process on its own free port.
That isolates the server's in-memory state between tests,
and it lets the suite run without a fixed port.
Every one of them binds the loopback interface,
so a test run is never reachable from another machine.

`DsServiceServer` starts and stops the process, not the harness,
so the fixtures cannot drift from the helper the client library ships.
Startup waits for the port to accept a TCP connection.
Startup then makes one read-only call,
which confirms that the service is registered and answers.
Teardown terminates the process.
If the process does not exit within the grace period `DsServiceServer` allows,
teardown kills it
(`TERMINATE_TIMEOUT_S` in `python/ds_service_client/server.py`).
That grace period stays above `SHUTDOWN_GRACE_S` in `cpp/server/main.cpp`,
so the helper never kills a server that is still draining its calls.

## Conventions

- After changing C++, check it with clangd.
    Pass `--compile-commands-dir` explicitly.
    Do not update the top-level `compile_commands.json` symlink.
- `.clang-format` in the repository root sets the C++ formatting.
- After changing Python, format it with black, then check it with pyright.
    `pyproject.toml` configures both.
    It excludes the generated `_ext.pyi` from the pyright check,
    though pyright still reads it to resolve imports,
    and black does not format it.
    pyright needs a build with the Python module,
    or it cannot resolve `ds_service_client._ext`.
- Use semantic line breaks in documentation, block comments,
    and docstrings.
- The gate for a change runs in this order:
    clang-format and black, then clangd and pyright,
    then a build, `ctest` and `python -m pytest`.

## Versioning

`scripts/update-version.sh <version>`
sets every version string in the repository:
`cpp/server/main.cpp`, `CMakeLists.txt`, `pyproject.toml`,
and `conanfile.py`.
Set them through the script rather than by hand.
The header of the script states what it edits,
what it gives `CMakeLists.txt`,
and when it refuses to edit anything.

## The dependency graph is built at task_add

`task_add` resolves every id in `parent_task_ids` before it adds the row,
and refuses the whole request if one of them is unknown.
That rule is what keeps the graph acyclic:
a parent must already exist,
so no task can name itself or close a loop through its own descendants.
Nothing after `task_add` adds an edge,
so no later call has to look for a cycle.

Each row carries the two halves of the graph,
`TaskTable::pending_parents` and `TaskTable::children`:

- `task_done` decrements the count on each child of the row it finishes,
    and enqueues a child whose count reaches zero while it is still `Waiting`.
    A row reported failed releases nothing:
    it hands its children the failure instead.
- `task_cancel` walks `children` from the row it cancels
    and cancels everything it reaches.
    `TaskManager::propagate_to_children` is that walk,
    and a failing `task_done` runs it with `Failed` instead.

A row is registered with a parent only while that parent is unfinished,
because a parent that has already finished has nothing left to release.
A row naming the same parent twice is registered with it twice,
which is what keeps the count and the releases in step.

## Known limitations

The server never reclaims `TaskTable` rows.
A task keeps its row for the life of the server process,
even after it reaches `Finished`, `Failed` or `Canceled`.
A long-lived server therefore holds one row per task ever added,
rather than one per task still outstanding.
`task_search_id` walks that table,
so its cost grows the same way.

Compaction is not a local change.
The index of a row is its address,
and `TaskManager::task_index`, every queue entry in `TaskManager::queue`,
every entry in `TaskTable::children`
and every entry in `TaskTable::terminal_origin` hold that index.
If you move a row, you rewrite all four.

The server does not reclaim dead queue entries either.
A heap cannot erase an entry from the middle.
`task_get` therefore discards dead entries only as it pops them,
and it never pops an entry that sorts below the live work.
`task_set_priority` is the way to accumulate them.
A rise in a task's priority leaves an entry at the old, lower value,
and a busy queue never reaches that entry.
A client that reprioritizes on a loop therefore grows the heap
by one entry per call per queue.

The user-facing consequences of both are in
[about the task queue](explanation/the-task-queue.md).
