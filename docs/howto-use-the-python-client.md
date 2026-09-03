# How to use the Python client

`ds_service_client` is a Python 3.12+ library
that wraps the generated gRPC stubs
and presents the server's data structures as ordinary methods
on a `DsServiceClient` object.

## Installing

```sh
pip install ds-service-client
```

Or, from a checkout of this repository:

```sh
pip install .
```

## Connecting

```python
from ds_service_client import DsServiceClient

client = DsServiceClient("127.0.0.1:5051")
```

If `DsServiceClient()` is constructed without an address,
it reads the server address from the `DS_SERVER_ADDRESS` environment variable.

The constructor also takes a `timeout` (seconds, default 300),
which is applied as the deadline of every RPC the client makes.
`client.close()` closes the underlying gRPC channel.

It is also a context manager,
which closes the channel on the way out
whether the block ends normally or raises:

```python
with DsServiceClient("127.0.0.1:5051") as client:
    client.map_set("greeting", b"hello")
```

## Errors

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

So a missing key raises `KeyError`,
and a bad regular expression or an over-sized message raises `ValueError`.
Any other status reaches the caller as a raw `grpc.RpcError`.

`task_get` is the one exception to the `NOT_FOUND` row:
no work ready raises `NoTaskAvailable`, which is not a `TimeoutError`.
A worker loop can therefore sleep and retry on `NoTaskAvailable`
and still fail against a server it cannot reach,
which raises `TimeoutError`.

`TaskStateError` comes from `task_done`
for a task that is not `Running`,
or one that is held by a different worker,
and from `task_get_worker_id` for a task that is not `Running`.
A cancelled task is the exception on `task_done`:
that call succeeds,
but the task stays `Canceled` and the output is discarded.

`MutexNotHeld` is the other exception to a table row.
`mutex_release` raises it when the caller is not the mutex's holder --
including a mutex that is already free, or that does not exist --
because a mutex belongs to the `worker_id` that acquired it.
`mutex_get_worker_id` raises it for a mutex that exists but is free;
a key that does not exist at all raises `KeyError` there.

A worker loop therefore looks like this:

```python
import time

from ds_service_client import NoTaskAvailable, TaskStateError

while True:
    try:
        task = client.task_get(worker_id="worker-a", queue="work")
    except NoTaskAvailable:
        time.sleep(1)
        continue
    # A TimeoutError here means the server is unreachable, and propagates.

    output = do_the_work(task)

    try:
        client.task_done(task.task_id, worker_id="worker-a", output=output)
    except TaskStateError:
        # The task is not this worker's to complete; drop the result.
        pass
```

## Usage

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

`mutex_acquire` is the one method with no RPC of its own:
it retries `mutex_try_acquire` in a loop with the same `worker_id`,
sleeping between attempts,
and raises `TimeoutError` once `timeout` seconds have elapsed.
With `timeout=None` (the default) it retries forever.

## Temporary servers

`DsServiceServer` runs a private `ds-service` process
for as long as the object lives.
The process starts as soon as the object is constructed.

```python
from ds_service_client import DsServiceClient, DsServiceServer

with DsServiceServer("lo") as server:
    server.wait_until_ready()          # blocks until the port accepts connections

    with DsServiceClient(server.address) as client:
        client.map_set("greeting", b"hello")
```

Leaving the `with` block calls `close()`,
which sends `SIGTERM`, waits for the grace period set by
`TERMINATE_TIMEOUT_S` in `ds_service_client/server.py`,
and then sends `SIGKILL`.
Call `close()` directly when not using it as a context manager.

The constructor takes one required argument and two optional ones:

| Argument | Default | Meaning |
| --- | --- | --- |
| `interface` | required | Network interface whose IPv4 address the server binds. |
| `port` | a free ephemeral port | Port the server binds; `0` means the same as leaving it out. |
| `ds_service_bin` | `$DS_SERVICE_BIN`, else `ds-service` | How to start the server. |

The interface decides who can reach the server:
`lo` for this machine only,
`eth0` or `ib0` for other machines on that network.
The server is never bound to a wildcard address,
so `server.address` -- `<ip of the interface>:<port>` --
is what every client connects to, local or remote:

```python
server = DsServiceServer("eth0")
server.address   # -> "172.17.0.2:45999", the address to hand to remote clients
                 # -> the InfiniBand address, had this been "ib0" on a cluster node
```

An interface that does not exist on this machine,
or that exists with no IPv4 address on it,
raises `ValueError` from the constructor,
before any server process is started.
`server.host` is the resolved address on its own.

`ds_service_bin` and `DS_SERVICE_BIN` may hold a **whole command**,
not just a path -- `docker run --rm --network host ds-service`
works as well as `/usr/bin/ds-service`.
`--address <host>:<port>` is appended to whatever is given,
and the result is split with `shlex.split`:
quoting is understood, but shell syntax is not --
put that in a script of your own and name the script here.

`wait_until_ready(timeout=30)` polls until the port accepts a TCP connection.
It raises `RuntimeError` if the process exits first,
and `TimeoutError` if the server is not listening within `timeout` seconds.
It only reports a server ready while our own process is still running.

See the [data-structure-reference.md](data-structure-reference.md)
for what each data structure and RPC does.
