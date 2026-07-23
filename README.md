# ds-service: Yet Another Data Structure Server

![Futuristic banner image.](misc/banner-image.png "Futuristic banner image.")

`ds-service` is a small, in-memory data structure server that is accessible via [gRPC](https://grpc.io/).

`ds-service` runs a single server process
that holds shared state in memory
and lets many distributed clients and workers coordinate using it.

Presently, it provides four things:
- **A key-value store** -- a shared `string -> bytes` store
    for passing data between processes.
- **A task queue** -- a priority-based work queue
    that distributes tasks to workers,
    tracks their state,
    and requeues tasks whose workers have died or stalled.
- **A journal store** -- append-only, ordered logs of binary entries.
- **A time series store** -- append-only series of timestamped
    floating-point values.

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
| `MapSearchKey(pattern)` | Return every key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

Values are opaque bytes, so callers are free to store whatever serialization
they like (JSON, pickle, protobuf, raw binary).

`MapSearchKey` matches keys against an [RE2](https://github.com/google/re2)
regular expression. The match is unanchored, so a key matches when any substring
of it matches the pattern; anchor with `^` and `$` to match a whole key.
Matching keys come back in unspecified order, so sort them if you need a stable
one. The search walks every key in the map while holding the global lock, which
is fine for the map sizes this server is meant for but is worth keeping in mind
if a map grows very large.

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

## The journal store

A key-to-journal store, where each journal is an append-only, ordered list of
opaque binary entries identified by a `string` key.

| RPC | Description |
| --- | --- |
| `JournalSize(key)` | Return the number of entries in the journal. A journal that does not exist has size `0`. |
| `JournalRead(key, start, end)` | Return the entries in the half-open index range `[start, end)`. |
| `JournalAppend(key, value)` | Append a single entry to the journal, creating it if it does not exist. |

`JournalRead` uses half-open ranges, so `JournalRead(key, 0, JournalSize(key))`
returns the whole journal. The range is clamped silently to the journal's
bounds: reading past the end returns only the entries that exist, and a range
with `start >= end` (or a journal that does not exist) returns an empty list --
neither is an error.

## The time series store

A key-to-series store, where each series is an append-only list of data points
identified by a `string` key. Each point carries a floating-point `value`, a
`datetime`, and an integer `step`.

| RPC | Description |
| --- | --- |
| `TimeSeriesAppend(key, value, datetime, step)` | Append a point to the series, creating it if it does not exist. `step` is optional and defaults to `0`. Returns `INVALID_ARGUMENT` if `datetime` does not parse. |
| `TimeSeriesGet(key, start_time, end_time, start_step, end_step)` | Return the points of a series, in append order, filtered by the given bounds. A key that does not exist returns an empty list. |

`datetime` is an ISO 8601 UTC datetime string. Both the `Z` form
(`2024-01-02T03:04:05Z`) and the offset form produced by Python's
`datetime.isoformat()` (`2024-01-02T03:04:05+00:00`) are accepted, as is a
non-UTC offset (converted to UTC) or a bare datetime (interpreted as UTC).
Fractional seconds are preserved to microsecond resolution. Datetimes returned
by `TimeSeriesGet` are normalized to the `Z` form.

All four bounds on `TimeSeriesGet` are optional. `start_time` and `start_step`
are inclusive lower bounds; `end_time` and `end_step` are exclusive upper
bounds. An unset bound imposes no restriction, and the bounds combine: a point
is returned only if it satisfies every bound provided. Points always come back
in the order they were appended, never sorted by time or step.

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

# Find keys by regular expression
client.map_set("run/1", b"...")
client.map_set("run/2", b"...")
assert sorted(client.map_search_key("^run/")) == ["run/1", "run/2"]

# Task queue
client.task_add("job-1", queue="work", priority=1.0, function=b"...", input=b"...")

task = client.task_get(worker_id="worker-a", queue="work")
# ... do the work ...
client.task_done(task.task_id, output=b"result")

status = client.task_status("job-1")
assert status.state == TaskState.Complete

# Time series
from datetime import datetime, timezone

client.time_series_append("loss", 0.9, datetime.now(timezone.utc).isoformat(), step=0)
client.time_series_append("loss", 0.5, datetime.now(timezone.utc).isoformat(), step=1)

points = client.time_series_get("loss", start_step=1)  # points with step >= 1
assert [p.value for p in points] == [0.5]
```

If `Client()` is constructed without an address, it reads the server address
from the `DS_SERVER_ADDRESS` environment variable.

## Optuna storage backend

[Optuna](https://optuna.org/)'s `JournalStorage` records a study as an
append-only log and rebuilds state by replaying it -- a natural fit for the
journal store. `ds_service_client.optuna_storage` provides a journal backend
that keeps a study's log in a ds-service journal (and stores Optuna's snapshots
in the key-value map), so many workers can share one distributed study by
pointing at the same server and `name`.

```sh
pip install ".[optuna]"
```

```python
import optuna
from ds_service_client.optuna_storage import create_journal_storage

storage = create_journal_storage("127.0.0.1:5051", name="my-study")

study = optuna.create_study(study_name="my-study", storage=storage)
study.optimize(objective, n_trials=100)
```

Other workers construct a storage with the same `name` and call
`optuna.load_study(study_name="my-study", storage=...)` to join the search.

## Running the tests

The test suite (`tests/`) is an integration suite driven by
[pytest](https://pytest.org/): it starts a fresh `ds-service` process for each
test and drives it through the Python client. Build the server first (the tests
run the compiled binary), then:

```sh
pip install -e ".[test]"   # pytest + the client package
python -m pytest
```

The tests locate the server binary at `build/Release/ds-service` or
`build/build/Release/ds-service`; set `DS_SERVICE_BIN` to override.

## License

MIT -- see [LICENSE](LICENSE).
