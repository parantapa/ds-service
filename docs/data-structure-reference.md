# Data structure reference

The data structures `ds-service` provides,
and the RPCs that operate on each of them.

`misc/ds-service.proto` is the authoritative definition
of the wire format.
This document describes what each RPC does.

Every RPC touches a single data structure
and takes that structure's lock for the duration of the call.
Thus the server serializes operations on one structure
while operations on different structures can run concurrently.

The server persists no state.
When it stops, every structure is lost.
See [about the architecture](about-the-architecture.md)
for why the server is built this way.

The Python names for these operations
are the snake_case forms of the RPC names (`MapSet` -> `client.map_set`).
See the [Python client reference](python-client-reference.md).

## The key-value store

A flat `string -> bytes` key-value store.

| RPC | Description |
| --- | --- |
| `MapSet(key, value)` | Store `value` under `key`. `MapSet` overwrites any existing value. |
| `MapGet(key)` | Return the value for `key`, or `NOT_FOUND` if it is missing. |
| `MapSearchKey(pattern)` | Return every key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

Values are binary blobs,
so callers are free to store data using whatever serialization
they like (JSON, pickle, protobuf, raw binary).

`MapSearchKey` matches keys
against a [RE2](https://github.com/google/re2) regular expression.
The match is unanchored,
so a key matches when any substring of it matches the pattern.
Use `^` and `$` to anchor the match to a whole key.
`MapSearchKey` returns matching keys in unspecified order.
`MapSearchKey` is slow, because it walks every key in the map
while holding the map's lock.
The search blocks every other map operation while it runs.

Four other stores expose the same operation over their own key space:

- `JournalSearchKey`
- `TimeSeriesSearchKey`
- `MutexSearchKey`
- `CounterSearchKey`

Each has identical RE2 semantics and the same cost:
a walk over every key under that store's lock.
The task queue does the same over its task ids with `TaskSearchId`.

## The task queue

Tasks are units of work identified by a unique `task_id`.
Each task carries an opaque `function` and `input` payload,
a floating-point `priority`,
and one or more named queues to dispatch it from.
The set of queues is fixed at `TaskAdd`.

A task moves through the states `Ready` -> `Running` -> `Complete`,
or to `Canceled` from either `Ready` or `Running`.
`Complete` and `Canceled` are both final.
A task in either state never moves again.
No live task holds the fifth state, `Undefined`.
`TaskGetStatus` reports it for a `task_id` that does not exist.

| RPC | Description |
| --- | --- |
| `TaskAdd(task_id, queue, priority, function, input)` | Register a new task and enqueue it on each named queue. Returns `ALREADY_EXISTS` if the id is already known. |
| `TaskGet(worker_id, queue)` | Claim the highest-priority `Ready` task from the first queue that has one, mark it `Running` on behalf of `worker_id`, and return its payload. `TaskGet` tries the queues in the order given. Returns `NOT_FOUND` when none of them has work ready. |
| `TaskDone(task_id, output, worker_id)` | Mark a `Running` task `Complete` and store its output. Returns `NOT_FOUND` for an unknown `task_id`, and `FAILED_PRECONDITION` if the task is not `Running` or is held by a different worker. A `Canceled` task is accepted and left alone. |
| `TaskGetStatus(task_id...)` | Return the state of each requested task, in request order. An unknown `task_id` reports `Undefined` rather than being an error. |
| `TaskGetOutput(task_id)` | Return a single task's output. Returns `NOT_FOUND` if the task does not exist. A task that has not finished yet has empty output. |
| `TaskGetCountByState()` | Return how many tasks are currently in each of the `Ready`, `Running`, `Complete`, and `Canceled` states. Takes no arguments. |
| `TaskCancel(task_id)` | Move a `Ready` or `Running` task to `Canceled` and report `success = true`. A task that is already `Complete` or `Canceled` is left alone and reports `success = false`. Returns `NOT_FOUND` for an unknown `task_id`. |
| `TaskGetPriority(task_id)` | Return the task's current priority. Returns `NOT_FOUND` for an unknown `task_id`. |
| `TaskSetPriority(task_id, priority)` | Change the task's priority. A `Ready` task is moved within every queue it waits on. A task in any other state records the new priority but is never dispatched again. Returns `NOT_FOUND` for an unknown `task_id`. |
| `TaskGetWorkerId(task_id)` | Return the `worker_id` holding a `Running` task. Returns `NOT_FOUND` for an unknown `task_id`, and `FAILED_PRECONDITION` if the task is not `Running`. |
| `TaskSearchId(pattern)` | Return every `task_id` matching the regular expression `pattern`. `TaskSearchId` searches tasks in every state. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

Within a queue, higher `priority` values are dispatched first,
and tasks of equal priority are dispatched in the order they were added.
A task moved by `TaskSetPriority` counts as newly added at its new priority:
it goes behind the tasks with equal priority already waiting there.

`TaskGet` answers `NOT_FOUND` for a queue with no work ready.

A task belongs to the worker that claimed it,
and `TaskGetWorkerId` reports which one that is.
Only a `Running` task has a holder to report:

- No worker claimed a `Ready` task.
- A worker handed a `Complete` task back when it finished.
- `TaskCancel` drops the record.

The server refuses `TaskDone` from any other worker
with `FAILED_PRECONDITION`.
The server refuses `TaskDone` the same way
on a task that is neither `Running` nor `Canceled`.
The call records no output and reports no success.

`TaskCancel` withdraws a task that has not finished.
`TaskCancel` moves a `Ready` task to `Canceled` before anybody claims it,
and takes a `Running` task away from the worker that holds it.
`TaskCancel` never puts a task back on a queue,
and it never touches a task that has already finished:
`success = false` says the task was left exactly as it was.

`TaskCancel` does not notify the worker that holds the task.
That worker can still call `TaskDone`.
The call succeeds, the task stays `Canceled`,
and the server discards the output.
`TaskDone` on a `Canceled` task is accepted from any worker,
because the server returns on the canceled state before it checks ownership.
`TaskCancel` also drops the record of which worker held the task.

A worker that dies mid-task leaves its task `Running`
for the life of the server.
Nothing hands it to another worker,
no RPC returns a task to `Ready`,
and `TaskAdd` refuses a `task_id` that already exists.
`TaskCancel` retires such a task.

`TaskSearchId` searches the task ids,
the key space the task queue has,
with the same RE2 semantics as `MapSearchKey`.
`TaskSearchId` searches every task the server knows about,
whatever state it is in.
Task rows are never reclaimed,
so the walk covers every task ever added
rather than the ones still outstanding.

See [about the task queue](about-the-task-queue.md)
for why the queue behaves this way,
and [how to write a worker](howto-write-a-worker.md)
for driving it from Python.

## The journal store

A store of journals, each identified by a `string` key.
Each journal is an append-only, ordered list of opaque binary entries.

| RPC | Description |
| --- | --- |
| `JournalSize(key)` | Return the number of entries in the journal. A journal that does not exist has size `0`. |
| `JournalRead(key, start, end)` | Return the entries in the half-open index range `[start, end)`. |
| `JournalAppend(key, value)` | Append a single entry to the journal. `JournalAppend` creates the journal if it does not exist. |
| `JournalSearchKey(pattern)` | Return every journal key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

`JournalRead` uses half-open ranges,
so `JournalRead(key, 0, JournalSize(key))` returns the whole journal.
`JournalRead` clamps the range silently to the journal's bounds,
and reports no error.
A read past the end returns only the entries that exist.
A range with `start >= end` returns an empty list,
and so does a journal that does not exist.

## The time series store

A store of series, each identified by a `string` key.
Each series is an append-only list of data points.
Each point carries a floating-point `value`, a `datetime`,
and an integer `step`.

| RPC | Description |
| --- | --- |
| `TimeSeriesAppend(key, value, datetime, step)` | Append a point to the series. `TimeSeriesAppend` creates the series if it does not exist. `step` is optional and defaults to `0`. Returns `INVALID_ARGUMENT` if `datetime` does not parse. |
| `TimeSeriesGet(key, start_time, end_time, start_step, end_step)` | Return the points of a series, in append order, filtered by the given bounds. A key that does not exist returns an empty list. Returns `INVALID_ARGUMENT` if `start_time` or `end_time` does not parse. |
| `TimeSeriesSearchKey(pattern)` | Return every series key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

`datetime` is a UTC datetime string in ISO 8601 format.
`TimeSeriesAppend` accepts four forms:

- The `Z` form, `2024-01-02T03:04:05Z`.
- The offset form that Python's `datetime.isoformat()` produces,
  `2024-01-02T03:04:05+00:00`.
- A non-UTC offset, which it converts to UTC.
- A bare datetime, which it reads as UTC.

`TimeSeriesAppend` keeps fractional seconds to microsecond resolution.
`TimeSeriesGet` normalizes the datetimes it returns to the `Z` form.

All four bounds on `TimeSeriesGet` are optional.
`start_time` and `start_step` are inclusive lower bounds.
`end_time` and `end_step` are exclusive upper bounds.
An unset bound, or an empty time string, imposes no restriction,
and the bounds combine:
`TimeSeriesGet` returns a point only if it satisfies every bound provided.
Points always come back in the order they were appended,
never sorted by `datetime` or `step`.

## Named mutexes

A map of named locks,
for coordinating exclusive access to a resource across workers.
A mutex is identified by a `string` key
and is either free or held by a `worker_id`.

| RPC | Description |
| --- | --- |
| `MutexTryAcquire(key, worker_id)` | Try once to acquire the mutex. `MutexTryAcquire` creates the mutex if it does not exist. Returns `true` if this call acquired it, recording `worker_id` as its holder, and `false` if it is already held. The holder can be any worker, `worker_id` included, because the lock is not reentrant. |
| `MutexRelease(key, worker_id)` | Release a mutex held by `worker_id`. `MutexRelease` clears the recorded holder. Returns `FAILED_PRECONDITION` if `worker_id` is not the holder. That covers a mutex held by another worker, one that is already free, and one that does not exist. |
| `MutexGetWorkerId(key)` | Return the `worker_id` holding the mutex. Returns `NOT_FOUND` if the key does not exist, and `FAILED_PRECONDITION` if it exists but is free. |
| `MutexSearchKey(pattern)` | Return every mutex key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

A mutex belongs to the `worker_id` that acquired it,
and only that worker can release it.
The lock is not reentrant:
the holder asking again is told the mutex is held, like anybody else.
The `worker_id` is taken at face value:
these are cooperative locks.

A key exists from the first `MutexTryAcquire` that names it,
whether or not that call acquired it.
The key is never removed.
`MutexRelease` frees the mutex and does not delete the key.
Neither a refused release nor `MutexGetWorkerId` creates anything.

`MutexGetWorkerId` tells a missing key from a free one.
A key that nobody named is `NOT_FOUND`,
and a key that exists with no holder is `FAILED_PRECONDITION`.
`MutexRelease` returns `FAILED_PRECONDITION` for both.

Mutexes have no expiry.
A worker that acquires a mutex and then dies leaves it held
for the life of the server.

The Python clients add a waiting `mutex_acquire`
on top of `MutexTryAcquire`.
See the [Python client reference](python-client-reference.md).

## Counters

A `string -> uint64` map of named counters
that hand out successive integers.
They are useful for unique ids or sequence numbers across workers.

| RPC | Description |
| --- | --- |
| `CounterGetNextValue(key)` | Return the next value of the counter. `CounterGetNextValue` creates the counter if it does not exist. The first call for a key returns `1`, and each subsequent call returns the previous value plus one. |
| `CounterGetCurrentValue(key)` | Return the counter's current value without changing it, or `0` if it does not exist. Read-only: it never creates the counter. |
| `CounterSearchKey(pattern)` | Return every counter key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

`CounterGetNextValue` both creates and advances a counter.
The first `CounterGetNextValue` for a key creates the counter and returns `1`.
`CounterGetCurrentValue` only reads:
it returns the value the last `CounterGetNextValue` handed out,
and leaves the counter untouched.
An unused counter reports `0`.

Because the counters' lock serializes every counter operation,
concurrent callers always receive distinct, gap-free values.
Counters are held in memory only,
so a server restart resets every counter.
The next value is `1` again.
