# ds-service: Yet Another Data Structure Server

![Futuristic banner image.](misc/banner-image.png "Futuristic banner image.")

`ds-service` is a small, in-memory data structure server that is accessible via [gRPC](https://grpc.io/).

`ds-service` runs a single server process
that holds shared state in memory
and lets many distributed clients and workers coordinate using it.
Use it when several processes -- on one machine or across a cluster --
need to hand work to each other,
share intermediate results,
take turns on a resource,
or agree on a number,
and the state only has to live as long as the run does.

Presently, it provides six data structures:
- **A key-value store** -- a shared `string -> bytes` store
    for passing data between processes.
- **A task queue** -- a priority-based work queue
    that distributes tasks to workers and tracks their state.
- **A journal store** -- append-only, ordered logs of binary entries.
- **A time series store** -- append-only series of
    timestamped floating-point values.
- **Named mutexes** -- worker-owned locks
    for coordinating exclusive resource access across workers.
- **Counters** -- named monotonic counters
    that hand out successive integers.

Each of these is a separate key space with its own set of RPCs.

## Installation

The server is a single statically linked binary.
Grab the latest release, make it executable,
and put it somewhere on your `PATH`:

```sh
curl -sSL -o ds-service \
    https://github.com/parantapa/ds-service/releases/latest/download/ds-service
chmod +x ds-service
```

It is linked against musl with no dynamic dependencies,
so it runs on any x86-64 Linux host.
To pin a version, name its tag instead of `latest`:
`.../releases/download/v5.0.0/ds-service`.

The Python client comes from PyPI:

```sh
pip install ds-service-client
```

To build the server from source instead,
see [how to build the server](docs/howto-build-the-server.md).

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

    client.task_add("job-1", queue="work", priority=1.0, function=b"greet", input=b"world")

    task = client.task_get(worker_id="worker-a", queue="work")
    client.task_done(task.task_id, worker_id="worker-a", output=b"hello world")

    assert client.task_get_output("job-1") == b"hello world"
```

## Documentation

| Document | What it covers |
| --- | --- |
| [Tutorial: run your first tasks](docs/tutorial-your-first-tasks.md) | Start a server, store a value, and take a task from `Ready` to `Complete`. Start here. |
| [How to build the server](docs/howto-build-the-server.md) | Requirements, the Conan + CMake build, installing, running, and the static musl build. |
| [How to write a worker](docs/howto-write-a-worker.md) | The claim-work-report loop, mutexes around shared resources, progress reporting, and the asyncio variant. |
| [How to run the tests](docs/howto-run-the-tests.md) | The pytest integration suite and pointing it at the binary. |
| [Data structure reference](docs/data-structure-reference.md) | Every RPC, its arguments and error statuses, and the exact semantics of each data structure. |
| [Python client reference](docs/python-client-reference.md) | `DsServiceClient` and `DsServiceClientAsync`: constructors, method names, the gRPC-status-to-exception mapping, and examples. |
| [Server helper reference](docs/server-helper-reference.md) | `DsServiceServer`, which runs a private `ds-service` process for the life of the object. |
| [About the architecture](docs/about-the-architecture.md) | The three pieces, why state is not persisted, one lock per structure, and why there are two Python clients. |
| [About the task queue](docs/about-the-task-queue.md) | Task ownership, what cancelling does and does not do, and why there is no fault tolerance. |
| [About the static musl build](docs/about-the-static-musl-build.md) | Why the static image exists and why its Conan profile differs. |
| [Developer notes](docs/developer-notes.md) | Working on `ds-service` itself: the source map, the generated code workflow, the test harness, conventions, versioning, and known limitations. |

## License

MIT. See [LICENSE](LICENSE).
