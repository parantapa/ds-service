# Python client reference

`ds_service_client` is a Python 3.12+ library
over the C++ client, which it carries in its extension module.
Its one run-time dependency is `ifaddr`.
It presents the server's data structures as ordinary methods
on a `DsServiceClient` object,
or on a `DsServiceClientAsync` object for asyncio callers.

This document describes the client library.
For what each operation does, see
the [data structure reference](data-structure.md).
For the process helper that starts a server,
see the [server helper reference](server-helper.md).

## `DsServiceClient`

```python
from ds_service_client import DsServiceClient

client = DsServiceClient("127.0.0.1:5051")
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `address` | `$DS_SERVER_ADDRESS` | `<host>:<port>` of the server, or `grpc://<host>:<port>`. Both select gRPC, the only transport today. The constructor raises `KeyError` when neither the argument nor the variable is set, and `ValueError` for an address that names an unknown transport. |
| `timeout` | `300` | Seconds, applied as the deadline of every call the client makes. |

The constructor does not contact the server.
An unreachable server fails the first call instead.

`client.close()` closes the client.
Calls in flight fail, and so does every later call.
The object is also a context manager,
which closes the client when the block exits,
whether the block ends normally or raises:

```python
with DsServiceClient("127.0.0.1:5051") as client:
    client.map_set("greeting", b"hello")
```

A call attempted after `close()` raises `RuntimeError`.
A second `close()` does nothing.

### Method names

Every method is one operation of the [data structure reference](data-structure.md),
with one addition:
`mutex_acquire` is not an operation of its own.
It retries `mutex_try_acquire` in a loop with the same `worker_id`,
and sleeps between attempts.
It raises `TimeoutError` once `timeout` seconds elapse.
With `timeout=None` (the default) it retries forever.

Two methods take either one task id or a list of them:

- `task_get_status` returns one state for a single string,
    and a list of states for a list.
- `task_add` takes `parent_task_ids` as one id or a list of ids.

### Return types

Most methods return plain Python values:
`bytes` for a payload, `str` for a key or an id,
`int`, `float`, `bool`, or a list of them.
Four types come from the extension module `ds_service_client._ext`:

| Type | Returned by | Attributes |
| --- | --- | --- |
| `TaskState` | `task_get_status` | An `enum.IntEnum`: `Waiting`, `Ready`, `Running`, `Finished`, `Failed`, `Canceled`, `Undefined`. |
| `TaskGetResponse` | `task_get` | `task_id` (`str`), `function` and `input` (`bytes`). |
| `TaskGetCountByStateResponse` | `task_get_count_by_state` | `waiting`, `ready`, `running`, `finished`, `failed`, `canceled` (`int`). |
| `TimeSeriesDataPoint` | `time_series_get` | `value` (`float`), `datetime` (`str`), `step` (`int`). |

The last three are read-only, and compare equal when their attributes do.
They are plain value objects,
so they have no methods that serialize them.

## `DsServiceClientAsync`

`DsServiceClientAsync` is the same API for asyncio.
It has the same method names, the same arguments,
and the same exceptions as the [exceptions table](#exceptions).
The caller awaits every call instead of blocking.
Every example in this document works against it by awaiting each call.

```python
import asyncio

from ds_service_client import DsServiceClientAsync


async def main() -> None:
    async with DsServiceClientAsync("127.0.0.1:5051") as client:
        await client.map_set("greeting", b"hello")
        assert await client.map_get("greeting") == b"hello"

        # Independent calls can be in flight at the same time.
        first, second = await asyncio.gather(
            client.counter_get_next_value("ids"),
            client.counter_get_next_value("ids"),
        )
        assert {first, second} == {1, 2}


asyncio.run(main())
```

The constructor takes one argument beyond those of `DsServiceClient`:

| Argument | Default | Meaning |
| --- | --- | --- |
| `max_workers` | `None` | The most calls in flight at once. Each call waits on a thread of the client's own pool. `None` takes the default of `concurrent.futures.ThreadPoolExecutor`, which grows with `os.cpu_count()`. |

Three things differ from `DsServiceClient`:

- `close()` is a coroutine, so it is `await client.close()`,
    and the context manager is `async with`, not `with`.
    `close()` also releases the pool's threads.
- A call beyond `max_workers` waits for a free thread before it starts.
- Canceling the coroutine that awaits a call does not cancel the call.
    It runs on until it completes or its deadline passes.

The constructor needs no running event loop,
so a client can be made at import time and used by any loop.

`mutex_acquire` waits with `asyncio.sleep`,
so only the coroutine that called it waits
while the rest of the loop keeps running.
`timeout` still bounds the whole loop, sleeps included.

The two clients are separate classes rather than one class with two modes.
See [about the architecture](../explanation/the-architecture.md#two-python-clients-one-api).

## Exceptions

The client translates each failed call into an ordinary Python exception.
The second column names the error code the client receives for the failure.

| Failure | Error code | Python exception |
| --- | --- | --- |
| The key, task or mutex does not exist | `NotFound` | `KeyError`, or `NoTaskAvailable` from `task_get` |
| The task id is already known | `AlreadyExists` | `ValueError` |
| A bad regular expression or datetime | `InvalidArgument` | `ValueError` |
| A message larger than 64 MiB | `MessageTooLarge` | `ValueError` |
| The state or the holder refuses the operation | `FailedPrecondition` | `TaskStateError`, or `MutexNotHeld` from `mutex_release` and `mutex_get_worker_id` |
| The server cannot be reached | `Unavailable` | `TimeoutError` |
| The call outlives its deadline | `DeadlineExceeded` | `TimeoutError` |
| The client is closed | `Closed` | `RuntimeError` |
| Anything else | any other | `TransportError` |

A missing key raises `KeyError` where the call needs the key to exist,
and a bad regular expression or an oversized message raises `ValueError`.
Each of these exceptions chains from the extension module's own error,
which `__cause__` holds.

A call that waits can be interrupted.
Ctrl-C raises `KeyboardInterrupt` within about 100 ms,
and cancels the call.
This holds only for a `DsServiceClient` call on the main thread.
A call on any other thread, and every `DsServiceClientAsync` call,
runs on until it completes or its deadline passes.

`task_get` raises `NoTaskAvailable` when no work is ready.
It is not a `TimeoutError`.

`task_done` raises `TaskStateError` for a task that is not `Running`,
or one that a different worker holds.
`task_get_worker_id` raises it for a task that is not `Running`.
A canceled task is the exception on `task_done`:
that call succeeds,
but the task stays `Canceled` and the server ignores the output.

`mutex_release` raises `MutexNotHeld`
when the caller is not the mutex's holder.
That covers a mutex that is already free and one that does not exist.
`mutex_get_worker_id` raises it for a mutex that exists but is free.
A key that does not exist at all raises `KeyError` there.

`NoTaskAvailable`, `TaskStateError`, `MutexNotHeld`, `TransportError` and `TaskState`
are importable from `ds_service_client`.

## Threads and processes

One client is safe to use from several threads at once.
Each call releases the GIL while it waits,
so other threads keep running.

A client does not survive `fork()`.
Once a process has created a client,
every call on an inherited client in a child it forks raises `RuntimeError`,
and so does constructing a new client there.
A child forked before the process created any client is unaffected.
With `multiprocessing`, the `spawn` and `forkserver` start methods avoid this,
and so does creating the first client inside the worker:

```python
import multiprocessing

from ds_service_client import DsServiceClient


def work(n: int) -> int:
    with DsServiceClient("127.0.0.1:5051") as client:
        return client.counter_get_next_value("ids")


if __name__ == "__main__":
    with multiprocessing.get_context("spawn").Pool(4) as pool:
        print(pool.map(work, range(8)))
```

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
client.task_add("job-1", parent_task_ids=[], queue="work", priority=1.0, function=b"...", input=b"...")

task = client.task_get(worker_id="worker-a", queue="work")
# It is Running now, and belongs to the worker that claimed it.
assert client.task_get_worker_id(task.task_id) == "worker-a"
# ... do the work ...
# worker_id must be the one that claimed the task.
client.task_done(task.task_id, worker_id="worker-a", output=b"result")

# Poll the state of one or more tasks; an unknown id reports Undefined.
# A single string returns one state; a list returns a list of states.
assert client.task_get_status("job-1") == TaskState.Finished
assert client.task_get_status(["job-1", "ghost"]) == [
    TaskState.Finished,
    TaskState.Undefined,
]
assert client.task_get_output("job-1") == b"result"

# Aggregate counts across all tasks in the system.
counts = client.task_get_count_by_state()
assert (
    counts.waiting,
    counts.ready,
    counts.running,
    counts.finished,
    counts.failed,
    counts.canceled,
) == (0, 0, 0, 1, 0, 0)

# A second task, this one never run.
client.task_add("job-2", parent_task_ids=[], queue="work", priority=1.0, function=b"...", input=b"...")

# Read and change the priority of a task that already exists.
# A Ready task is moved within the queues it waits on.
assert client.task_get_priority("job-2") == 1.0
client.task_set_priority("job-2", 5.0)

# Withdraw a task that has not ended yet.
# True if this call moved it to Canceled,
# False if it was already Finished, Failed or Canceled.
# Canceling a task cancels every task waiting on it.
assert client.task_cancel("job-2") is True
assert client.task_get_status("job-2") == TaskState.Canceled
assert client.task_get_output("job-2") == b"Task canceled"

# A task that waits for others.
# parent_task_ids takes one id or a list of them,
# and every parent must already exist.
client.task_add("job-3", parent_task_ids=[], queue="work", priority=1.0, function=b"...", input=b"...")
client.task_add(
    "job-4",
    parent_task_ids="job-3",
    queue="work",
    priority=1.0,
    function=b"...",
    input=b"...",
)

# job-4 is Waiting, so no queue offers it while job-3 is unfinished.
assert client.task_get_status("job-4") == TaskState.Waiting

task = client.task_get(worker_id="worker-a", queue="work")
assert task.task_id == "job-3"
client.task_done("job-3", worker_id="worker-a", output=b"result")

# Finishing the last parent releases it.
assert client.task_get_status("job-4") == TaskState.Ready

# A worker that ends in an error reports the task Failed instead,
# which fails every task waiting on it.
# job-4 is still Ready, so job-5 takes a higher priority to be claimed first.
client.task_add("job-5", parent_task_ids=[], queue="work", priority=2.0, function=b"...", input=b"...")
client.task_add(
    "job-6",
    parent_task_ids="job-5",
    queue="work",
    priority=1.0,
    function=b"...",
    input=b"...",
)

client.task_get(worker_id="worker-a", queue="work")
client.task_done("job-5", worker_id="worker-a", output=b"traceback", failed=True)

assert client.task_get_status("job-5") == TaskState.Failed
assert client.task_get_output("job-5") == b"traceback"
assert client.task_get_status("job-6") == TaskState.Failed
assert client.task_get_output("job-6") == b"Dependency failed (task_id=job-5)"

# Find task ids by regular expression, whatever state the tasks are in.
assert sorted(client.task_search_id("^job-")) == [
    "job-1",
    "job-2",
    "job-3",
    "job-4",
    "job-5",
    "job-6",
]

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
