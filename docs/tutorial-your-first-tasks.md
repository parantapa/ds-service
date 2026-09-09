# Tutorial: run your first tasks through ds-service

By the end of this tutorial you will have a `ds-service` server running
on your own machine,
a Python session talking to it,
and a task that you submit from one place and complete from another.

We will download a ready-made binary
and start it from Python,
which is the quickest way to have something to talk to.
All you need is Python 3.12 or newer, on an x86-64 Linux machine.

## Step 1: get the server

Download the latest release into the directory you are working in:

```sh
curl -sSL -o ds-service \
    https://github.com/parantapa/ds-service/releases/latest/download/ds-service
chmod +x ds-service
```

Check that it runs:

```sh
./ds-service --version
```

It prints a version number, such as `5.0.0`.
You will not have to start it yourself;
Python will do that in step 3.

The binary is statically linked,
so there is nothing else to install and nothing to put on your `PATH`.

## Step 2: install the client

```sh
pip install ds-service-client
```

Now tell the library where the binary you just downloaded is:

```sh
export DS_SERVICE_BIN=./ds-service
```

The library looks at `DS_SERVICE_BIN` first,
and falls back to a `ds-service` on your `PATH`.

## Step 3: start a server

Open a Python session and start a server of your own:

```python
from ds_service_client import DsServiceServer

server = DsServiceServer("lo")
server.wait_until_ready()
server.address
```

The last line prints something like `'127.0.0.1:45999'`.
Notice the port: you never chose it.
The helper picked a free one,
which is what lets you run several of these side by side later.

`"lo"` is the loopback interface,
so this server is reachable from this machine and nowhere else.

Keep this session open.
The server runs for as long as the `server` object lives.

## Step 4: store and read a value

Connect a client to the address you just printed:

```python
from ds_service_client import DsServiceClient

client = DsServiceClient(server.address)

client.map_set("greeting", b"hello")
client.map_get("greeting")
```

You will see `b'hello'` come back.

Note the `b`.
Values are bytes, never `str`.
Try it without the prefix and see what happens:

```python
client.map_set("greeting", "hello")
```

That raises a `TypeError` from the client before anything reaches the server.
Anything you want to store has to be encoded first --
`"hello".encode()`, `json.dumps(...).encode()`, `pickle.dumps(...)`.

Ask for a key that was never set:

```python
client.map_get("nothing-here")
```

That raises `KeyError`.
This is worth pausing on:
the server answered with a gRPC `NOT_FOUND` status,
and the client turned it into the exception you would expect
from an ordinary Python mapping.
Every error you meet from here on arrives the same way.

## Step 5: add a task

Now the part `ds-service` exists for.
Submit a unit of work:

```python
client.task_add(
    "job-1",
    queue="work",
    priority=1.0,
    function=b"greet",
    input=b"world",
)
```

Nothing runs.
`function` and `input` are opaque bytes;
the server never looks inside them
and has no idea how to execute anything.
It is holding the task until somebody asks for it.

Check on it:

```python
client.task_get_status("job-1")
```

A bare `0` comes back.
States are a protobuf enum, and its members are plain integers,
so compare against `TaskState` rather than reading the number:

```python
from ds_service_client import TaskState

client.task_get_status("job-1") == TaskState.Ready
```

`True` -- the task is waiting on the queue named `work`.

When you want to read a state rather than test it, ask for its name:

```python
TaskState.Name(client.task_get_status("job-1"))
```

`'Ready'`.
We will use that form from here on, because it is easier to follow.

## Step 6: claim it and finish it

Claim the task the way a worker would:

```python
task = client.task_get(worker_id="worker-a", queue="work")
task.task_id, task.function, task.input
```

You get back `('job-1', b'greet', b'world')`.

Look at the status again:

```python
TaskState.Name(client.task_get_status("job-1"))
```

`'Running'`.
Claiming a task changed its state.
The task also has an owner:

```python
client.task_get_worker_id("job-1")
```

`'worker-a'` -- the name you passed to `task_get`.

Now do the work and report back.
The result is bytes again:

```python
result = b"hello world"
client.task_done("job-1", worker_id="worker-a", output=result)

TaskState.Name(client.task_get_status("job-1"))
client.task_get_output("job-1")
```

`'Complete'`, and `b'hello world'`.

You have taken a task through its whole life:
`Ready`, `Running`, `Complete`.

## Step 7: watch ownership being enforced

Add a second task and claim it as `worker-a`:

```python
client.task_add("job-2", queue="work", priority=1.0, function=b"greet", input=b"again")
task = client.task_get(worker_id="worker-a", queue="work")
```

Now try to finish it as somebody else:

```python
client.task_done("job-2", worker_id="worker-b", output=b"stolen")
```

That raises `TaskStateError`.
`worker-b` does not hold the task,
so the server refuses to let it overwrite the result of the worker that does.
Finish it properly:

```python
client.task_done("job-2", worker_id="worker-a", output=b"hello again")
```

Ask for more work when the queue is empty:

```python
client.task_get(worker_id="worker-a", queue="work")
```

That raises `NoTaskAvailable`.
Notice that this is not a `TimeoutError`:
an empty queue and an unreachable server are different problems,
and a worker loop needs to sleep on the first and give up on the second.

## Step 8: see the whole picture

```python
client.task_get_count_by_state()
```

```
ready: 0
running: 0
complete: 2
canceled: 0
```

Two complete, nothing else.

And find your tasks by pattern:

```python
sorted(client.task_search_id("^job-"))
```

`['job-1', 'job-2']`.
Both are finished,
and both are still there.
The server remembers every task it has ever been given.

## Step 9: stop the server

```python
client.close()
server.close()
```

Start a fresh server and look for your work:

```python
server = DsServiceServer("lo")
server.wait_until_ready()
client = DsServiceClient(server.address)
client.task_search_id("^job-")
```

An empty list.
Nothing is persisted:
every value, task, journal and counter lived in the memory of a process
that no longer exists.
This is the single most important thing to remember about `ds-service`,
and it is much easier to remember once you have watched it happen.

```python
client.close()
server.close()
```

## What you have done

You started a server, stored a value,
and moved a task through `Ready`, `Running` and `Complete` from two sides.
You saw the server refuse a `task_done` from the wrong worker,
distinguish an empty queue from an unreachable server,
and lose everything on restart.

Try repeating steps 5 and 6 a few times with different ids and priorities;
the loop becomes familiar quickly,
and it is the same loop every real worker runs.

When you are ready to write one for real, see
[how to write a worker](howto-write-a-worker.md).
For everything the server can hold besides tasks --
journals, time series, mutexes, counters --
see the [data structure reference](data-structure-reference.md).
