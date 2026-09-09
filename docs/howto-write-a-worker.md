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

For the full client API, see the
[Python client reference](python-client-reference.md).

## Connect

```sh
pip install ds-service-client
```

```python
from ds_service_client import DsServiceClient

client = DsServiceClient("127.0.0.1:5051")
```

If you deploy the same code to many machines,
leave the address out and set `DS_SERVER_ADDRESS` in the environment instead.

Use the client as a context manager when the worker has a definite end,
so the channel is closed whether the block finishes or raises:

```python
with DsServiceClient("127.0.0.1:5051") as client:
    ...
```

## Run the claim-work-report loop

Pick a `worker_id` that is unique to this process.
The server uses it to decide who owns a claimed task,
and it takes the name at face value,
so two processes sharing a name can complete each other's tasks.

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

Two failures are worth separating in this loop.
`NoTaskAvailable` means the queue is empty and the worker should wait.
`TimeoutError` means the server could not be reached,
so let it propagate rather than retrying forever against a dead server.

If the worker should drain a queue and exit rather than wait for more,
break out of the loop on `NoTaskAvailable` instead of sleeping.

To take work from several queues in priority order,
pass them in the order you want them tried:

```python
task = client.task_get(worker_id="worker-a", queue=["urgent", "work"])
```

`TaskStateError` on `task_done` means the task stopped being yours
while you were working on it,
which in practice means somebody cancelled it.
Dropping the result is usually right,
because a cancelled task discards its output anyway.

## Guard a resource that only one worker may touch

If the work needs exclusive access to something outside the server --
a file, a device, an external service --
take a named mutex around it.
To give up rather than wait when it is busy:

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

Always release in a `finally`.
A mutex has no expiry,
so a worker that dies while holding one blocks the others
until the server is restarted.
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

Both create the key on first append,
so no setup is needed before the worker starts.

## Hand out unique names

If workers need ids that no other worker will produce -- output filenames,
run numbers, task ids for work they submit themselves --
use a counter rather than random values:

```python
run_id = client.counter_get_next_value("runs")
```

Counter values are serialized under a single lock,
so concurrent callers always get distinct, gap-free numbers.

## Run the loop on an event loop

If the worker is already asyncio-based,
use `DsServiceClientAsync`.
The method names, arguments and exceptions are the same,
and every call is awaited:

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

Construct it inside a coroutine, not at import time.
See the
[Python client reference](python-client-reference.md#dsserviceclientasync)
for the differences from the blocking client.

## Know what the queue will not do for you

A worker that dies mid-task leaves that task `Running` forever.
Nothing reassigns it, and `task_add` refuses to reuse the id,
so recovery means cancelling the task and submitting the work under a new id.
Decide who does that -- it is not the queue.
See [about the task queue](about-the-task-queue.md).
