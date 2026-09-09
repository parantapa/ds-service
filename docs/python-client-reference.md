# Python client reference

`ds_service_client` is a Python 3.12+ library
that wraps the generated gRPC stubs
and presents the server's data structures as ordinary methods
on a `DsServiceClient` object,
or on a `DsServiceClientAsync` object for asyncio callers.

This document describes the client library.
For what each underlying RPC does, see the
[data structure reference](data-structure-reference.md).
For the process helper that starts a server,
see the [server helper reference](server-helper-reference.md).

## Installing

```sh
pip install ds-service-client
```

From a checkout of this repository:

```sh
pip install .
```

## `DsServiceClient`

```python
from ds_service_client import DsServiceClient

client = DsServiceClient("127.0.0.1:5051")
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `address` | `$DS_SERVER_ADDRESS` | `<host>:<port>` of the server. A `KeyError` is raised when neither the argument nor the variable is set. |
| `timeout` | `300` | Seconds, applied as the deadline of every RPC the client makes. |

`client.close()` closes the underlying gRPC channel.
The object is also a context manager,
which closes the channel on the way out
whether the block ends normally or raises:

```python
with DsServiceClient("127.0.0.1:5051") as client:
    client.map_set("greeting", b"hello")
```

An RPC attempted after `close()` raises `ValueError`.

### Method names

Every method is the snake_case form of the RPC it calls
(`MapSet` -> `client.map_set`),
with one addition:
`mutex_acquire` has no RPC of its own.
It retries `MutexTryAcquire` in a loop with the same `worker_id`,
sleeping between attempts,
and raises `TimeoutError` once `timeout` seconds have elapsed.
With `timeout=None` (the default) it retries forever.

## Exceptions

The client translates gRPC status codes into ordinary Python exceptions:

| gRPC status | Python exception |
| --- | --- |
| `NOT_FOUND` | `KeyError`, or `NoTaskAvailable` from `task_get` |
| `ALREADY_EXISTS` | `ValueError` |
| `INVALID_ARGUMENT` | `ValueError` |
| `RESOURCE_EXHAUSTED` | `ValueError` |
| `FAILED_PRECONDITION` | `TaskStateError`, or `MutexNotHeld` from the `mutex_*` methods |
| `UNAVAILABLE` | `TimeoutError` |
| `DEADLINE_EXCEEDED` | `TimeoutError` |

A missing key raises `KeyError`,
and a bad regular expression or an over-sized message raises `ValueError`.
Any other status reaches the caller as a raw `grpc.RpcError`.

`NoTaskAvailable` is raised by `task_get` when no work is ready.
It is not a `TimeoutError`.

`TaskStateError` is raised by `task_done`
for a task that is not `Running`,
or one that is held by a different worker,
and by `task_get_worker_id` for a task that is not `Running`.
A cancelled task is the exception on `task_done`:
that call succeeds,
but the task stays `Canceled` and the output is discarded.

`MutexNotHeld` is raised by `mutex_release`
when the caller is not the mutex's holder,
which covers a mutex that is already free and one that does not exist.
`mutex_get_worker_id` raises it for a mutex that exists but is free;
a key that does not exist at all raises `KeyError` there.

`NoTaskAvailable`, `TaskStateError`, `MutexNotHeld` and `TaskState`
are importable from `ds_service_client`.

## Examples

```python
from ds_service_client import DsServiceClient, TaskState

client = DsServiceClient("127.0.0.1:5051")  # or set DS_SERVER_ADDRESS and call DsServiceClient()

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
# It is Running now, and belongs to the worker that claimed it.
assert client.task_get_worker_id(task.task_id) == "worker-a"
# ... do the work ...
# worker_id must be the one that claimed the task.
client.task_done(task.task_id, worker_id="worker-a", output=b"result")

# Poll the state of one or more tasks; an unknown id reports Undefined.
# A single string returns one state; a list returns a list of states.
assert client.task_get_status("job-1") == TaskState.Complete
assert client.task_get_status(["job-1", "ghost"]) == [
    TaskState.Complete,
    TaskState.Undefined,
]
assert client.task_get_output("job-1") == b"result"

# Aggregate counts across all tasks in the system.
counts = client.task_get_count_by_state()
assert (counts.ready, counts.running, counts.complete, counts.canceled) == (0, 0, 1, 0)

# A second task, this one never run.
client.task_add("job-2", queue="work", priority=1.0, function=b"...", input=b"...")

# Read and change the priority of a task that already exists.
# A task still waiting is moved within the queues it waits on.
assert client.task_get_priority("job-2") == 1.0
client.task_set_priority("job-2", 5.0)

# Withdraw a task that has not finished yet.
# True if this call moved it to Canceled,
# False if it was already Complete or Canceled.
assert client.task_cancel("job-2") is True
assert client.task_get_status("job-2") == TaskState.Canceled

# Find task ids by regular expression, whatever state the tasks are in.
assert sorted(client.task_search_id("^job-")) == ["job-1", "job-2"]

# Journal
client.journal_append("events", b"started")
client.journal_append("events", b"finished")

size = client.journal_size("events")
assert client.journal_read("events", 0, size) == [b"started", b"finished"]

assert client.journal_search_key("^events$") == ["events"]

# Time series
from datetime import datetime, timezone

client.time_series_append("loss", 0.9, datetime.now(timezone.utc).isoformat(), step=0)
client.time_series_append("loss", 0.5, datetime.now(timezone.utc).isoformat(), step=1)

points = client.time_series_get("loss", start_step=1)  # points with step >= 1
assert [p.value for p in points] == [0.5]

assert client.time_series_search_key("^loss$") == ["loss"]

# Named mutex
# The mutex belongs to the worker_id that acquired it,
# and only that worker can release it.
if client.mutex_try_acquire("resource-a", worker_id="worker-a"):
    try:
        ...  # exclusive section
        assert client.mutex_get_worker_id("resource-a") == "worker-a"
    finally:
        client.mutex_release("resource-a", worker_id="worker-a")

# Or block until acquired, giving up after 30 seconds
client.mutex_acquire("resource-a", worker_id="worker-a", timeout=30.0)
try:
    ...  # exclusive section
finally:
    client.mutex_release("resource-a", worker_id="worker-a")

assert client.mutex_search_key("^resource-") == ["resource-a"]

# Counter
assert client.counter_get_next_value("ids") == 1
assert client.counter_get_next_value("ids") == 2

assert client.counter_get_current_value("ids") == 2  # read-only peek
assert client.counter_get_current_value("unused") == 0

assert client.counter_search_key("^ids$") == ["ids"]
```

## `DsServiceClientAsync`

`DsServiceClientAsync` is the same API over `grpc.aio`:
the same method names, the same arguments,
and the same exceptions as the table above,
with every RPC awaited instead of blocking the caller.
Every example above works against it by awaiting each call.

```python
import asyncio

from ds_service_client import DsServiceClientAsync


async def main() -> None:
    async with DsServiceClientAsync("127.0.0.1:5051") as client:
        await client.map_set("greeting", b"hello")
        assert await client.map_get("greeting") == b"hello"

        # Independent RPCs can be in flight at the same time.
        first, second = await asyncio.gather(
            client.counter_get_next_value("ids"),
            client.counter_get_next_value("ids"),
        )
        assert {first, second} == {1, 2}


asyncio.run(main())
```

Three things differ from `DsServiceClient`:

- `close()` is a coroutine, so it is `await client.close()`,
    and the context manager is `async with`, not `with`.
- The constructor must run with an event loop already running --
    inside a coroutine, not at import time --
    because `grpc.aio` binds the channel
    to the loop that is current when the channel is created.
- An RPC attempted after `close()` raises `grpc.aio.UsageError`,
    where the blocking client raises `ValueError`.

`mutex_acquire` waits with `asyncio.sleep`,
so only the coroutine that called it waits
while the rest of the loop keeps running.
`timeout` still bounds the whole loop, sleeps included.

The two clients are separate classes rather than one class with two modes;
see [about the architecture](about-the-architecture.md#two-python-clients-one-api).
