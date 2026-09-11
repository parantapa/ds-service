# How to write a worker

A worker is a process that claims tasks from a `ds-service` queue,
runs them, and reports the results back.
This guide covers the loop itself
and the coordination problems that come with running several of them.

It assumes you have a server to talk to.
If you do not, see
[how to build the server](howto-build-the-server.md),
or start a private one with
[`DsServiceServer`](server-helper-reference.md).

For the full client API, see
the [Python client reference](python-client-reference.md).

## Connect

```sh
pip install ds-service-client
```

```python
from ds_service_client import DsServiceClient

client = DsServiceClient("127.0.0.1:5051")
```

If you deploy the same code to many machines,
omit the address and set `DS_SERVER_ADDRESS` in the environment instead.

When the worker has a definite end,
use the client as a context manager.
The client then closes the channel whether the block finishes or raises:

```python
with DsServiceClient("127.0.0.1:5051") as client:
    ...
```

## Run the claim-work-report loop

Pick a `worker_id` that is unique to this process.
The server uses it to decide who owns a claimed task,
and takes the name at face value.
Two processes that share a name can complete each other's tasks.

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

Separate two failures in this loop.
`NoTaskAvailable` means the queue is empty and the worker must wait.
`TimeoutError` means the client cannot reach the server.
Let it propagate.
Do not retry forever against a dead server.

To drain the queue and exit rather than wait,
break out of the loop on `NoTaskAvailable`.

To take work from several queues in priority order,
pass them in the order you want the server to try them:

```python
task = client.task_get(worker_id="worker-a", queue=["urgent", "work"])
```

`TaskStateError` on `task_done` means the task was no longer `Running`
under your `worker_id`,
so something else already completed it.
Drop the result.
The server already stored the output that the other call recorded.
`task_done` on a canceled task succeeds, and the server discards its output.

## Guard a resource that only one worker can touch

Some work needs exclusive access to something outside the server:
a file, a device, or an external service.
Take a named mutex around that access.
To return rather than wait when the mutex is busy:

```python
if client.mutex_try_acquire("resource-a", worker_id="worker-a"):
    try:
        ...  # exclusive section
    finally:
        client.mutex_release("resource-a", worker_id="worker-a")
```

To wait for it, with a bound on how long:

```python
client.mutex_acquire("resource-a", worker_id="worker-a", timeout=30.0)
try:
    ...  # exclusive section
finally:
    client.mutex_release("resource-a", worker_id="worker-a")
```

Release the mutex in a `finally`.
A mutex has no expiry.
A worker that dies with the mutex held blocks every other worker
until you restart the server.
Use the same `worker_id` for both calls:
only the holder can release.

## Report progress while the work runs

To let something else watch the run, append as you go.
Use a journal for events and a time series for numbers:

```python
from datetime import datetime, timezone

client.journal_append(f"log/{task.task_id}", b"started")
client.time_series_append("loss", 0.9, datetime.now(timezone.utc).isoformat(), step=0)
```

Both calls create the key on first append,
so the worker needs no setup before it starts.

## Generate unique ids

Some workers need ids that no other worker produces:
output filenames, run numbers, or task ids for work they submit.
Use a counter for those ids rather than random values:

```python
run_id = client.counter_get_next_value("runs")
```

The server serializes counter values under a single lock,
so concurrent callers always get distinct, gap-free numbers.

## Run the loop on an event loop

If the worker is already asyncio-based,
use `DsServiceClientAsync`.
The method names, arguments and exceptions are the same,
and you await every call:

```python
import asyncio

from ds_service_client import DsServiceClientAsync, NoTaskAvailable

async def worker() -> None:
    async with DsServiceClientAsync("127.0.0.1:5051") as client:
        while True:
            try:
                task = await client.task_get(worker_id="worker-a", queue="work")
            except NoTaskAvailable:
                await asyncio.sleep(1)
                continue
            ...
```

Construct `DsServiceClientAsync` inside a coroutine, not at import time.
See
the [Python client reference](python-client-reference.md#dsserviceclientasync)
for the differences from the blocking client.

## Know what the queue will not do for you

Decide who recovers a task that a dead worker left `Running`.
The queue does not do it for you.
A worker that dies mid-task leaves that task `Running` forever.
Nothing reassigns it, and `task_add` refuses to reuse the id.
To recover the work, cancel the task and submit it under a new id.
See [about the task queue](about-the-task-queue.md).
