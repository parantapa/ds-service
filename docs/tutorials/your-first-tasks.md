# Run your first tasks through ds-service

By the end of this tutorial
we will have a `ds-service` server running on our own machine.
A Python session talks to it,
and we submit a task in one place and finish it in another.

We download a ready-made binary and start it from Python.
That is the quickest way to have something to talk to.
All we need is Python 3.12 or newer, on an x86-64 Linux machine.

## Step 1: get the server

Download the latest release into the working directory:

```sh
curl -sSL -o ds-service \
    https://github.com/parantapa/ds-service/releases/latest/download/ds-service
chmod +x ds-service
```

Check that it runs:

```sh
./ds-service --version
```

It prints a version number.
We will not start it by hand.
Python starts it in step 3.

The binary is statically linked,
so there is nothing else to install and nothing to put on our `PATH`.

## Step 2: install the client

```sh
pip install ds-service-client
```

Now tell the library where the binary we downloaded is:

```sh
export DS_SERVICE_BIN=./ds-service
```

The [server helper reference](../reference/server-helper.md)
gives the other ways the library finds the binary.

## Step 3: start a server

In the same terminal, start Python:

```sh
python
```

The export from step 2 reaches this session, and only this one.
Start a server of our own:

```python
from ds_service_client import DsServiceServer

server = DsServiceServer("lo")
server.wait_until_ready()
server.address
```

The last line prints something like `'127.0.0.1:45999'`.
Notice the port: we never chose it.
The helper picked a free one,
which is what lets us run several servers side by side later.

`"lo"` is the loopback interface,
so this server is reachable from this machine and nowhere else.

Keep this session open.
The server runs until we call `server.close()`.

## Step 4: store and read a value

Connect a client to the address we printed:

```python
from ds_service_client import DsServiceClient

client = DsServiceClient(server.address)

client.map_set("greeting", b"hello")
client.map_get("greeting")
```

We get `b'hello'` back.

Note the `b`.
Values are bytes, never `str`.
Try it without the prefix.
See what happens:

```python
client.map_set("greeting", "hello")
```

That raises a `TypeError` from the client before anything reaches the server.
We encode anything we want to store.
For `"hello"`, that is `"hello".encode()`.

Ask for a key that was never set:

```python
client.map_get("nothing-here")
```

That raises `KeyError`.
The server answered with a gRPC `NOT_FOUND` status.
The client turned it into the exception
that an ordinary Python mapping raises.
Every error we meet from here on arrives the same way.

## Step 5: add a task

Now the part `ds-service` exists for.
Submit a unit of work:

```python
client.task_add(
    "job-1",
    parent_task_ids=[],
    queue="work",
    priority=1.0,
    function=b"greet",
    input=b"world",
)
```

Nothing runs.
`parent_task_ids=[]` says that the task depends on no other task.
`function` and `input` are opaque bytes.
The server never looks inside them
and has no idea how to execute anything.
It holds the task until somebody asks for it.

Check it:

```python
client.task_get_status("job-1")
```

A bare `1` comes back.
States are a protobuf enum, and its members are plain integers.
Compare against `TaskState` rather than reading the number:

```python
from ds_service_client import TaskState

client.task_get_status("job-1") == TaskState.Ready
```

`True`.
The task waits on the queue named `work`.

To read a state rather than test it, ask for its name:

```python
TaskState.Name(client.task_get_status("job-1"))
```

`'Ready'`.
We use that form for the rest of this tutorial,
because it is easier to follow.

## Step 6: claim it and finish it

Claim the task the way a worker does:

```python
task = client.task_get(worker_id="worker-a", queue="work")
task.task_id, task.function, task.input
```

We receive `('job-1', b'greet', b'world')`.

Look at the status again:

```python
TaskState.Name(client.task_get_status("job-1"))
```

`'Running'`.
The claim changed the state of the task.
The task also has an owner:

```python
client.task_get_worker_id("job-1")
```

`'worker-a'`, the name we passed to `task_get`.

Now do the work.
Then report the result.
The result is bytes again:

```python
result = b"hello world"
client.task_done("job-1", worker_id="worker-a", output=result)

TaskState.Name(client.task_get_status("job-1"))
client.task_get_output("job-1")
```

`'Finished'`, and `b'hello world'`.

We took a task through its whole life:
`Ready`, `Running`, `Finished`.

A worker can also report a task as `Failed`.
For how, see [how to write a worker](../how-to-guides/write-a-worker.md).

## Step 7: watch the server enforce ownership

Add a second task.
Then claim it as `worker-a`:

```python
client.task_add("job-2", parent_task_ids=[], queue="work", priority=1.0, function=b"greet", input=b"again")
task = client.task_get(worker_id="worker-a", queue="work")
```

Now try to finish it as somebody else:

```python
client.task_done("job-2", worker_id="worker-b", output=b"stolen")
```

That raises `TaskStateError`.
`worker-b` does not hold the task.
So the server refuses to let it overwrite the result of the owner.
Finish it properly:

```python
client.task_done("job-2", worker_id="worker-a", output=b"hello again")
```

When the queue is empty, ask for more work:

```python
client.task_get(worker_id="worker-a", queue="work")
```

That raises `NoTaskAvailable`.
Notice that this is not a `TimeoutError`.
An empty queue and an unreachable server are different problems.
A worker loop sleeps on the first and gives up on the second.

## Step 8: count the tasks

```python
client.task_get_count_by_state()
```

```
waiting: 0
ready: 0
running: 0
finished: 2
failed: 0
canceled: 0
```

Two finished, nothing else.

Now find our tasks by pattern:

```python
sorted(client.task_search_id("^job-"))
```

`['job-1', 'job-2']`.
Both are finished,
and both are still there.
The server remembers every task we give it.

## Step 9: stop the server

```python
client.close()
server.close()
```

Start a fresh server.
Then look for our work:

```python
server = DsServiceServer("lo")
server.wait_until_ready()
client = DsServiceClient(server.address)
client.task_search_id("^job-")
```

An empty list.
The server persists nothing.
Every value, task, journal and counter lived in the memory
of a process that no longer exists.
Remember this about `ds-service`, above everything else.
We just watched it happen.
For why the server is built this way, see
[about the architecture](../explanation/the-architecture.md).

```python
client.close()
server.close()
```

## What we did

We started a server, stored a value,
and moved a task through `Ready`, `Running` and `Finished` from two sides.
We also saw the server do three things:

- It refused a `task_done` from the wrong worker.
- It distinguished an empty queue from an unreachable server.
- It lost everything on restart.

Start a fresh server and client as in step 9.
Then repeat steps 5 and 6 a few times with different ids and priorities.
The loop becomes familiar quickly.
It is the same loop every real worker runs.

To write one for real, see
[how to write a worker](../how-to-guides/write-a-worker.md).
The server also holds journals, time series, mutexes and counters.
For those, see the [data structure reference](../reference/data-structure.md).
