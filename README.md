# ds-service: Yet Another Data Structure Server

![Futuristic banner image.](misc/banner-image.png "Futuristic banner image.")

`ds-service` is a small, in-memory data structure server that is accessible via [gRPC](https://grpc.io/).

`ds-service` runs a single server process
that holds shared state in memory
and lets many distributed clients and workers coordinate using it.

Presently, it provides two things:
- **A key-value store** -- a shared `string -> bytes` store
    for passing data between processes.
- **A task queue** -- a priority-based work queue
    that distributes tasks to workers,
    tracks their state,
    and requeues tasks whose workers have died or stalled.

It is designed as a lightweight coordination system for distributed batch jobs
(for example, when running calibration and projection workflows across many nodes of an HPC cluster),
where you need some shared data structures
and a way to hand work out to a pool of workers.

## Architecture

- **Server** (`cpp/ds-service.cpp`) -- a C++23 gRPC service.
    All state lives in memory and is guarded by a single global lock,
    so operations are serialized and consistent.
    State is **not** persisted; that is, when the server stops all data is lost.
- **Client** (`python/ds_service_client/`) -- a Python 3.12+ client library
    that wraps the generated gRPC stubs
    and translates gRPC status codes into ordinary Python exceptions
    (`KeyError`, `ValueError`, `TimeoutError`).
- **Interface** (`misc/ds-service.proto`) -- the protobuf/gRPC contract
    shared by both sides.

By default the server listens on `127.0.0.1:5051`.

## The key-value store

A flat `string -> bytes` key-value store.

| RPC | Description |
| --- | --- |
| `MapSet(key, value)` | Store `value` under `key`, overwriting any existing value. |
| `MapGet(key)` | Return the value for `key`, or `NOT_FOUND` if it is missing. |

Values are opaque bytes, so callers are free to store whatever serialization
they like (JSON, pickle, protobuf, raw binary).

## The task queue

Tasks are units of work identified by a unique `task_id`.
Each task carries an opaque `function` and `input` payload,
a floating-point `priority`,
and one or more named queues it should be dispatched from.
A task moves through three states: `Ready` → `Running` → `Complete`.

| RPC | Description |
| --- | --- |
| `TaskAdd(task_id, queue, priority, function, input)` | Register a new task and enqueue it on each named queue. Returns `ALREADY_EXISTS` if the id is already known. |
| `TaskGet(worker_id, queue)` | Claim the highest-priority `Ready` task from the first non-empty queue, mark it `Running`, and return its payload. Returns `UNAVAILABLE` when no work is ready. |
| `TaskDone(task_id, output)` | Mark a `Running` task `Complete` and store its output. |
| `TaskStatus(task_id)` | Return a task's current state and, once complete, its output. |
| `Requeue(timeout_s)` | Reset any task that has been `Running` longer than `timeout_s` back to `Ready` and re-enqueue it. |

Within a queue, higher `priority` values are dispatched first.
A worker polls `TaskGet` across the queues it cares about, runs the work,
and reports back with `TaskDone`.
`Requeue` provides fault tolerance:
if a worker crashes without completing its task,
a periodic `Requeue` call makes that task available to another worker.

## Building the server

Dependencies are managed with [Conan](https://conan.io/)
and the build is driven by CMake.

```sh
conan install . --build=missing
. build/Release/generators/conanbuild.sh
cmake -S . -B build/Release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake
cmake --build build/Release --parallel
```

A reproducible container build is defined in `scripts/apptainer/ds-service.def`.

### Running

```sh
ds-service --address 0.0.0.0:5051
```

Run `ds-service --help` for the full argument list.

## Python client

```sh
pip install .  # installs the ds-service-client package
```

```python
from ds_service_client import Client, TaskState

client = Client("127.0.0.1:5051")  # or set DS_SERVER_ADDRESS and call Client()

# Key-value map
client.map_set("greeting", b"hello")
assert client.map_get("greeting") == b"hello"

# Task queue
client.task_add("job-1", queue="work", priority=1.0, function=b"...", input=b"...")

task = client.task_get(worker_id="worker-a", queue="work")
# ... do the work ...
client.task_done(task.task_id, output=b"result")

status = client.task_status("job-1")
assert status.state == TaskState.Complete
```

If `Client()` is constructed without an address, it reads the server address
from the `DS_SERVER_ADDRESS` environment variable.

## License

MIT -- see [LICENSE](LICENSE).
