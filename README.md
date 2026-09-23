# ds-service: An in-memory data structure server

![Futuristic banner image.](misc/banner-image.png "Futuristic banner image.")

`ds-service` is a small, in-memory data structure server
that is accessible via [gRPC](https://grpc.io/).

`ds-service` runs a single server process
that holds shared state in memory
and lets many distributed clients and workers coordinate using it.
Use it when several processes must coordinate,
on one machine or across a cluster.
It covers four cases:

- The processes hand work to each other.
- The processes share intermediate results.
- The processes take turns on a resource.
- The processes agree on a number.

The state only has to live as long as the run does.

Presently, it provides six data structures:
a key-value store, a task queue, a journal store, a time series store,
named mutexes, and counters.
Each is a separate key space with its own set of RPCs,
described in the [data structure reference](docs/reference/data-structure.md).

## Installation

The server is a single statically linked binary.
Download the latest release, make it executable,
and put it somewhere on your `PATH`:

```sh
curl -sSL -o ds-service \
    https://github.com/parantapa/ds-service/releases/latest/download/ds-service
chmod +x ds-service
```

It links against musl with no dynamic dependencies,
so it runs on any x86-64 Linux host.

The Python client needs Python 3.12 or newer.
It comes from PyPI:

```sh
pip install ds-service-client
```

PyPI has a prebuilt wheel for x86-64 Linux.
On any other platform, pip builds the client from source,
which needs a C++23 toolchain and takes several minutes.

The C++ client comes from source only.
See the [C++ client reference](docs/reference/cpp-client.md).

To build the server from source instead,
see [how to build the server](docs/how-to-guides/build-the-server.md).

## Usage

Start a server:

```sh
ds-service --address 127.0.0.1:5051
```

Then, from any process that can reach it:

```python
from ds_service_client import DsServiceClient

with DsServiceClient("127.0.0.1:5051") as client:
    client.map_set("greeting", b"hello")
    assert client.map_get("greeting") == b"hello"

    client.task_add("job-1", parent_task_ids=[], queue="work", priority=1.0, function=b"greet", input=b"world")

    task = client.task_get(worker_id="worker-a", queue="work")
    client.task_done(task.task_id, worker_id="worker-a", output=b"hello world")

    assert client.task_get_output("job-1") == b"hello world"
```

## Upgrading from 6.x

Version 7.0.0 moves the Python client onto a C++ client,
which it carries in its extension module.
The wire format is unchanged,
so a 6.x client works with a 7.0.0 server, and the other way round.
These changes can break code written for 6.x:

- `grpcio` and `protobuf` are no longer dependencies.
    A failure that 6.x passed on as a raw `grpc.RpcError`
    now raises `TransportError`, from `ds_service_client`.
- `task_get`, `task_get_count_by_state` and `time_series_get`
    return read-only objects instead of protobuf messages.
    Their attributes are unchanged,
    but protobuf methods such as `SerializeToString()` are gone.
- `TaskState` is an `enum.IntEnum`.
    Use `state.name` and `TaskState["Ready"]`
    where 6.x code called `TaskState.Name()` and `TaskState.Value()`.
- A call on a closed client raises `RuntimeError` on both clients.
    6.x raised `ValueError`, or `grpc.aio.UsageError` on the async client.
- `DsServiceClientAsync` no longer needs a running event loop to be constructed.
    It runs each call on a thread pool, sized with the new `max_workers` argument.
- A client does not survive `fork()`.
    Every call in a process forked after a client was created
    raises `RuntimeError`.
    With `multiprocessing`, use the `spawn` or `forkserver` start method.
- The module-level internals of `ds_service_client.client` changed,
    among them `translate_grpc_error` and `GRPC_CLIENT_OPTIONS`,
    and the clients lost their `channel` and `stub` attributes.

See the [Python client reference](docs/reference/python-client.md)
for the details.

## Documentation

### User documentation

| Document | What it covers |
| --- | --- |
| [Run your first tasks through ds-service](docs/tutorials/your-first-tasks.md) | Start a server, store a value, and take a task from `Ready` to `Finished`. Start here. |
| [How to write a worker](docs/how-to-guides/write-a-worker.md) | The claim-work-report loop, mutexes around shared resources, progress reporting, and the asyncio variant. |
| [Data structure reference](docs/reference/data-structure.md) | Every RPC, its arguments and error statuses, and the exact semantics of each data structure. |
| [Python client reference](docs/reference/python-client.md) | `DsServiceClient` and `DsServiceClientAsync`: constructors, method names, return types, the mapping from a failure to an exception, threads and processes, and examples. |
| [C++ client reference](docs/reference/cpp-client.md) | `ds::connect` and `ds::Client`: headers, options, methods, errors, and an example. |
| [Server helper reference](docs/reference/server-helper.md) | `DsServiceServer`, which starts a private `ds-service` process and stops it on `close()`. |
| [About the architecture](docs/explanation/the-architecture.md) | The pieces and the transport boundary between them, why the server does not persist state, one lock per structure, and why there are two Python clients. |
| [About the task queue](docs/explanation/the-task-queue.md) | Task ownership, what canceling does and does not do, and why there is no fault tolerance. |
| [About the static musl build](docs/explanation/the-static-musl-build.md) | Why the static image exists and why its Conan profile differs. |

### Developer documentation

| Document | What it covers |
| --- | --- |
| [How to build the ds-service server](docs/how-to-guides/build-the-server.md) | Requirements, the Conan and CMake build, the options that select the server, the C++ client and the Python module, the tests, how to install and run the binary, and the static musl build. |
| [Developer notes](docs/developer-notes.md) | A map of the source, the build and the test suites, the generated code, the transport boundary, and the invariants that span C++ and Python. |

## License

MIT. See [LICENSE](LICENSE).
