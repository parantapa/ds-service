# Data structure reference

The data structures `ds-service` provides,
and the operations that act on each of them.

This document describes what each operation does.
It names each operation and its arguments the way both clients do.
It names each error by its error code, such as `NotFound`.
Each client reports these error codes as its own reference describes.
The [Python client](python-client.md#exceptions) raises an exception for each,
and the [C++ client](cpp-client.md#errors) throws a `ds::ClientError`
that carries the `ErrorCode` of the same name.

Every operation touches a single data structure
and holds that structure's lock while it reads or changes the structure.
Thus the server serializes operations on one structure
while operations on different structures can run concurrently.

The server persists no state.
When it stops, every structure is lost.
See [about the architecture](../explanation/the-architecture.md)
for why the server is built this way.

Each operation is a method of the same name on both clients
(`map_set` is `client.map_set` in Python and `client->map_set` in C++).
See the [Python client reference](python-client.md)
and the [C++ client reference](cpp-client.md).

## Key search

Each store has an operation that searches its own key space:

- `map_search_key`
- `journal_search_key`
- `time_series_search_key`
- `mutex_search_key`
- `counter_search_key`
- `task_search_id`, over the task ids of the task queue

Each one matches keys
against a [RE2](https://github.com/google/re2) regular expression.
The match is unanchored,
so a key matches when any substring of it matches the pattern.
`^` and `$` anchor the match to a whole key.
`map_search_key` returns matching keys in unspecified order.

Each search is slow,
because it walks every key in its store
while holding that store's lock.
`map_search_key` blocks every other map operation while it runs.

`task_search_id` searches every task the server knows about,
whatever state it is in.
Task rows are never reclaimed,
so the walk covers every task ever added
rather than the ones still outstanding.

## Key-value store

A flat `string -> bytes` key-value store.

| Operation | Description |
| --- | --- |
| `map_set(key, value)` | Store `value` under `key`. `map_set` overwrites any existing value. |
| `map_get(key)` | Return the value for `key`, or `NotFound` if it is missing. |
| `map_search_key(pattern)` | Return every key matching the regular expression `pattern`. Returns `InvalidArgument` if the pattern does not compile. |

Values are binary blobs,
so a value can hold data in any serialization,
such as JSON, pickle, protobuf or raw binary.

[Key search](#key-search) describes how `map_search_key` matches keys.

## Task queue

Tasks are units of work identified by a unique `task_id`.
Each task carries an opaque `function` and `input` payload,
a floating-point `priority`,
one or more named queues to dispatch it from,
and the ids of the tasks it waits for.
Both sets are fixed at `task_add`.

A task with a `Canceled` or `Failed` parent starts in that state,
as [Dependencies](#dependencies) describes.
Otherwise, a task with a parent that has not finished starts `Waiting`.
Every other task starts `Ready`.
A task moves through the states
`Waiting` -> `Ready` -> `Running` -> `Finished`,
and a worker reports `Failed` in place of `Finished`
for a task that ran and ended in an error.
A task reaches `Canceled` from any of the three states before it ends,
and `Failed` from `Waiting` when a task it depends on fails.
`Finished`, `Failed` and `Canceled` are all final.
A task in one of them never moves again.
No live task holds the seventh state, `Undefined`.
`task_get_status` reports it for a `task_id` that does not exist.

| Operation | Description |
| --- | --- |
| `task_add(task_id, parent_task_ids, queue, priority, function, input)` | Register a new task and enqueue it on each named queue. A task with a parent that is `Waiting`, `Ready` or `Running` starts `Waiting` and enters no queue yet, unless another parent is `Canceled` or `Failed`. Returns `AlreadyExists` if the id is already known, and `NotFound`, adding nothing, for a parent the server does not know. |
| `task_get_status(task_id)` | Return the state of each task in the list `task_id`, in list order. An unknown `task_id` reports `Undefined` rather than being an error. |
| `task_get_output(task_id)` | Return a single task's output. Returns `NotFound` if the task does not exist. A task that has not ended yet has empty output, a canceled task reports `Task canceled`, and a task failed by one it depends on reports `Dependency failed (task_id=...)`, naming the task whose run failed. |
| `task_get_count_by_state()` | Return how many tasks are currently in each of the `Waiting`, `Ready`, `Running`, `Finished`, `Failed` and `Canceled` states. Takes no arguments. |
| `task_cancel(task_id)` | Move a `Waiting`, `Ready` or `Running` task to `Canceled`, cancel every task waiting on it, and report `success = true`. A task that is already `Finished`, `Failed` or `Canceled` is left alone and reports `success = false`. Returns `NotFound` for an unknown `task_id`. |
| `task_get_priority(task_id)` | Return the task's current priority. Returns `NotFound` for an unknown `task_id`. |
| `task_set_priority(task_id, priority)` | Change the task's priority. A `Ready` task is moved within every queue it waits on. A `Waiting` task records the new priority and enters its queues at that priority when its parents finish. A task in any other state records the new priority but is never dispatched again. Returns `NotFound` for an unknown `task_id`. |
| `task_get_worker_id(task_id)` | Return the `worker_id` holding a `Running` task. Returns `NotFound` for an unknown `task_id`, and `FailedPrecondition` if the task is not `Running`. |
| `task_search_id(pattern)` | Return every `task_id` matching the regular expression `pattern`. `task_search_id` searches tasks in every state. Returns `InvalidArgument` if the pattern does not compile. |
| `task_get(worker_id, queue)` | Claim the highest-priority `Ready` task from the first queue that has one, mark it `Running` on behalf of `worker_id`, and return its payload. `task_get` tries the queues in the order given. Returns `NotFound` when none of them has work ready. |
| `task_done(task_id, worker_id, output, failed)` | Store a `Running` task's output and mark it `Finished`, or `Failed` when `failed` is true. Failing a task fails every task waiting on it. Returns `NotFound` for an unknown `task_id`, and `FailedPrecondition` if the task is not `Running` or is held by a different worker. A `Canceled` task is accepted and left alone. |

### Dispatch order

Within a queue, higher `priority` values are dispatched first,
and tasks of equal priority are dispatched in the order they were added.
A task moved by `task_set_priority` counts as newly added at its new priority:
it goes behind the tasks with equal priority already waiting there.

### Task ownership

A task belongs to the worker that claimed it,
and `task_get_worker_id` reports which one that is.
Only a `Running` task has a holder to report:

- No worker claimed a `Ready` task.
- A worker handed a `Finished` or `Failed` task back when it ended.
- `task_cancel` drops the record.

The server refuses `task_done` from any other worker
with `FailedPrecondition`.
The server refuses `task_done` the same way
on a task that is neither `Running` nor `Canceled`.
The call records no output and reports no success.

### Cancellation

`task_cancel` withdraws a task that has not finished.
`task_cancel` moves a `Ready` task to `Canceled` before anybody claims it,
and takes a `Running` task away from the worker that holds it.
`task_cancel` never puts a task back on a queue,
and it never touches a task that has already finished:
`success = false` says the task was left exactly as it was.

`task_cancel` does not notify the worker that holds the task.
That worker can still call `task_done`.
The call succeeds, the task stays `Canceled`,
and the server discards the output.
`task_done` on a `Canceled` task is accepted from any worker.
`task_cancel` also drops the record of which worker held the task.

### Dependencies

`parent_task_ids` names the tasks a task waits for.
A task enters its queues when the last of its parents reaches `Finished`,
and `task_get` never offers it before that.
It enters them at whatever priority it holds at that moment,
and counts as newly queued:
it waits behind the tasks of equal priority already there.

Every parent must exist when `task_add` names it.
A graph of tasks is therefore added parents first,
and no task can name itself or close a cycle.
`task_add` returns `NotFound` for a parent the server does not know,
and adds nothing at all in that case.

A canceled task never finishes, and neither does a failed one,
so the tasks waiting on either can never run.
The end of such a parent is passed down the graph:

- `task_cancel` cancels every task waiting on the task it cancels,
    and every task waiting on those, however deep the graph runs.
- `task_done` with `failed` fails them the same way.
- `task_add` adds a task with a `Canceled` parent as `Canceled`,
    and one with a `Failed` parent as `Failed`.
    A parent that failed decides this over one that was canceled.

`task_get_output` reports why such a task never ran.
A task canceled, whether by name or through a parent,
reports `Task canceled`.
A task failed through a parent reports
`Dependency failed (task_id=...)`,
naming the task whose own run failed,
not the parent it was waiting on.
A task that did run keeps whatever its worker reported,
whether it finished or failed.

### Dead workers

A worker that dies mid-task leaves its task `Running`
for the life of the server.
Nothing hands it to another worker,
no operation returns a task to `Ready`,
and `task_add` refuses a `task_id` that already exists.
`task_cancel` retires such a task.

See [about the task queue](../explanation/the-task-queue.md)
for why the queue behaves this way,
and [how to write a worker](../how-to-guides/write-a-worker.md)
for driving it from Python.

## Journal store

A store of journals, each identified by a `string` key.
Each journal is an append-only, ordered list of opaque binary entries.

| Operation | Description |
| --- | --- |
| `journal_size(key)` | Return the number of entries in the journal. A journal that does not exist has size `0`. |
| `journal_read(key, start, end)` | Return the entries in the half-open index range `[start, end)`. |
| `journal_append(key, value)` | Append a single entry to the journal. `journal_append` creates the journal if it does not exist. |
| `journal_search_key(pattern)` | Return every journal key matching the regular expression `pattern`. Returns `InvalidArgument` if the pattern does not compile. |

`journal_read` uses half-open ranges,
so `journal_read(key, 0, journal_size(key))` returns the whole journal.
`journal_read` clamps the range silently to the journal's bounds,
and reports no error.
A read past the end returns only the entries that exist.
A range with `start >= end` returns an empty list,
and so does a journal that does not exist.

## Time series store

A store of series, each identified by a `string` key.
Each series is an append-only list of data points.
Each point carries a floating-point `value`, a `datetime`,
and an integer `step`.

| Operation | Description |
| --- | --- |
| `time_series_append(key, value, datetime, step)` | Append a point to the series. `time_series_append` creates the series if it does not exist. `step` is optional and defaults to `0`. Returns `InvalidArgument` if `datetime` does not parse. |
| `time_series_get(key, start_time, end_time, start_step, end_step)` | Return the points of a series, in append order, filtered by the given bounds. A key that does not exist returns an empty list. Returns `InvalidArgument` if `start_time` or `end_time` does not parse. |
| `time_series_search_key(pattern)` | Return every series key matching the regular expression `pattern`. Returns `InvalidArgument` if the pattern does not compile. |

`datetime` is a UTC datetime string in ISO 8601 format.
`time_series_append` accepts four forms:

- The `Z` form, `2024-01-02T03:04:05Z`.
- The offset form that Python's `datetime.isoformat()` produces,
  `2024-01-02T03:04:05+00:00`.
- A non-UTC offset, which it converts to UTC.
- A bare datetime, which it reads as UTC.

`time_series_append` keeps fractional seconds to the nanosecond,
and the time bounds on `time_series_get` compare against that value.
`time_series_get` normalizes the datetimes it returns to the `Z` form,
and truncates their fractional seconds to the microsecond.
A datetime on a whole second comes back with no fractional part.

All four bounds on `time_series_get` are optional.
`start_time` and `start_step` are inclusive lower bounds.
`end_time` and `end_step` are exclusive upper bounds.
An unset bound, or an empty time string, imposes no restriction,
and the bounds combine:
`time_series_get` returns a point only if it satisfies every bound provided.
Points always come back in the order they were appended,
never sorted by `datetime` or `step`.

## Named mutexes

A map of named locks,
for coordinating exclusive access to a resource across workers.
A mutex is identified by a `string` key
and is either free or held by a `worker_id`.

| Operation | Description |
| --- | --- |
| `mutex_try_acquire(key, worker_id)` | Try once to acquire the mutex. `mutex_try_acquire` creates the mutex if it does not exist. Returns `true` if this call acquired it, recording `worker_id` as its holder, and `false` if it is already held. The holder can be any worker, `worker_id` included, because the lock is not reentrant. |
| `mutex_release(key, worker_id)` | Release a mutex held by `worker_id`. `mutex_release` clears the recorded holder. Returns `FailedPrecondition` if `worker_id` is not the holder. That covers a mutex held by another worker, one that is already free, and one that does not exist. |
| `mutex_get_worker_id(key)` | Return the `worker_id` holding the mutex. Returns `NotFound` if the key does not exist, and `FailedPrecondition` if it exists but is free. |
| `mutex_search_key(pattern)` | Return every mutex key matching the regular expression `pattern`. Returns `InvalidArgument` if the pattern does not compile. |

A mutex belongs to the `worker_id` that acquired it,
and only that worker can release it.
The lock is not reentrant:
the holder asking again is told the mutex is held, like anybody else.
The `worker_id` is taken at face value:
these are cooperative locks.

A key exists from the first `mutex_try_acquire` that names it,
whether or not that call acquired it.
The key is never removed.
`mutex_release` frees the mutex and does not delete the key.
Neither a refused release nor `mutex_get_worker_id` creates anything.

`mutex_get_worker_id` tells a missing key from a free one.
A key that nobody named is `NotFound`,
and a key that exists with no holder is `FailedPrecondition`.
`mutex_release` returns `FailedPrecondition` for both.

Mutexes have no expiry.
A worker that acquires a mutex and then dies leaves it held
for the life of the server.

The Python and C++ clients add a waiting `mutex_acquire`
on top of `mutex_try_acquire`.
See the [Python client reference](python-client.md).

## Counters

A `string -> uint64` map of named counters
that hand out successive integers,
for unique ids or sequence numbers across workers.

| Operation | Description |
| --- | --- |
| `counter_get_next_value(key)` | Return the next value of the counter. `counter_get_next_value` creates the counter if it does not exist. The first call for a key returns `1`, and each subsequent call returns the previous value plus one. |
| `counter_get_current_value(key)` | Return the counter's current value without changing it, or `0` if it does not exist. Read-only: it never creates the counter. |
| `counter_search_key(pattern)` | Return every counter key matching the regular expression `pattern`. Returns `InvalidArgument` if the pattern does not compile. |

`counter_get_next_value` both creates and advances a counter.
The first `counter_get_next_value` for a key creates the counter and returns `1`.
`counter_get_current_value` only reads:
it returns the value the last `counter_get_next_value` handed out,
and leaves the counter untouched.
An unused counter reports `0`.

Because the counters' lock serializes every counter operation,
concurrent callers always receive distinct, gap-free values.
Counters are held in memory only,
so a server restart resets every counter.
The next value is `1` again.
