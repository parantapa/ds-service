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
| [About the task queue](docs/explanation/the-task-queue.md) | Task ownership, what canceling does and does not do, how failure and cancellation pass to dependents, why there is no fault tolerance, and why the server reclaims nothing. |

### Developer documentation

| Document | What it covers |
| --- | --- |
| [How to build the ds-service server](docs/how-to-guides/build-the-server.md) | Requirements, the Conan and CMake build, the options that select the server, the C++ client and the Python module, the tests, how to install and run the binary, and the static musl build. |
| [About the static musl build](docs/explanation/the-static-musl-build.md) | Why the static image exists and why its Conan profile differs. |
| [Developer notes](docs/developer-notes.md) | A map of the source, the build and the test suites, the generated code, the transport boundary, and the invariants that span C++ and Python. |

## License

MIT. See [LICENSE](LICENSE).
