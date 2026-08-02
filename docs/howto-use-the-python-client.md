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
| `NOT_FOUND` | `KeyError` |
| `ALREADY_EXISTS` | `ValueError` |
| `INVALID_ARGUMENT` | `ValueError` |
| `RESOURCE_EXHAUSTED` | `ValueError` |
| `UNAVAILABLE` | `TimeoutError` |
| `DEADLINE_EXCEEDED` | `TimeoutError` |

So a missing key raises `KeyError`,
a bad regular expression or an over-sized message raises `ValueError`,
and `task_get` with no work ready raises `TimeoutError`.
Any other status reaches the caller as a raw `grpc.RpcError`.

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
# ... do the work ...
client.task_done(task.task_id, output=b"result")

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
assert (counts.ready, counts.running, counts.complete) == (0, 0, 1)

# Reset tasks that have been running for more than 300 seconds back to Ready.
client.task_requeue(300.0)

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
if client.mutex_try_acquire("resource-a"):
    try:
        ...  # exclusive section
    finally:
        client.mutex_release("resource-a")

# Or block until acquired, giving up after 30 seconds
client.mutex_acquire("resource-a", timeout=30.0)
try:
    ...  # exclusive section
finally:
    client.mutex_release("resource-a")

assert client.mutex_search_key("^resource-") == ["resource-a"]

# Counter
assert client.counter_get_next_value("ids") == 1
assert client.counter_get_next_value("ids") == 2

assert client.counter_get_current_value("ids") == 2  # read-only peek
assert client.counter_get_current_value("unused") == 0

assert client.counter_search_key("^ids$") == ["ids"]
```

`mutex_acquire` is the one method with no RPC of its own:
it retries `mutex_try_acquire` in a loop,
sleeping between attempts,
and raises `TimeoutError` once `timeout` seconds have elapsed.
With `timeout=None` (the default) it retries forever.

## Temporary servers

`DsServiceServer` runs a private `ds-service` process
for as long as the object lives.
The process starts as soon as the object is constructed.

```python
from ds_service_client import DsServiceClient, DsServiceServer

with DsServiceServer() as server:
    server.wait_until_ready()          # blocks until the port accepts connections

    with DsServiceClient(f"{server.connect_host}:{server.port}") as client:
        client.map_set("greeting", b"hello")
```

Leaving the `with` block calls `close()`,
which sends `SIGTERM`, waits ten seconds,
and then sends `SIGKILL`.
Call `close()` directly when not using it as a context manager.

The constructor takes three optional arguments:

| Argument | Default | Meaning |
| --- | --- | --- |
| `host` | `"0.0.0.0"` | IPv4 address the server binds. |
| `port` | a free ephemeral port | Port the server binds; `0` means the same as leaving it out. |
| `ds_service_bin` | `$DS_SERVICE_BIN`, else `ds-service` | How to start the server. |

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

To hand the address to clients on other machines,
use `get_address_by_interface`,
which pairs the port with an interface's IPv4 address:

```python
server.get_address_by_interface("eth0")   # -> "172.17.0.2:45999"
server.get_address_by_interface("ib0")    # -> the InfiniBand address, on a cluster node
```

An interface that does not exist on this machine 
(or has no IPv4 address) produces a warning
and falls back to `127.0.0.1`.
For a server bound to a wildcard address,
`server.connect_host` is the loopback address
to use from the same machine.

See the [data-structure-reference.md](data-structure-reference.md)
for what each data structure and RPC does.
