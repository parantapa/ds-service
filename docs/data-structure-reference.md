# Data structure reference

The data structures `ds-service` provides,
and the RPCs that operate on each of them.

`misc/ds-service.proto` is the authoritative definition
of the wire format;
this document describes what each RPC does.

Every RPC touches a single data structure
and takes that structure's lock for the duration of the call.
Thus operations on one structure are serialized
while operations on different structures may run concurrently.
No state is persisted -- when the server stops, all of it is lost.

The Python names for these operations are the snake_case forms of the
RPC names (`MapSet` -> `client.map_set`);
see [howto-use-the-python-client.md](howto-use-the-python-client.md).

## The key-value store

A flat `string -> bytes` key-value store.

| RPC | Description |
| --- | --- |
| `MapSet(key, value)` | Store `value` under `key`, overwriting any existing value. |
| `MapGet(key)` | Return the value for `key`, or `NOT_FOUND` if it is missing. |
| `MapSearchKey(pattern)` | Return every key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

Values are binary blobs,
so callers are free to store data using whatever serialization
they like (JSON, pickle, protobuf, raw binary).

`MapSearchKey` matches keys against a
[RE2](https://github.com/google/re2) regular expression.
The match is unanchored,
so a key matches when any substring of it matches the pattern;
`^` and `$` can be used to anchor the match to a whole key.
Matching keys are returned in unspecified order.
Searching for keys is **slow** as the search walks every key in the map
while holding the map's lock.
This blocks other map operations during this period.

The journal, time series, mutex, and counter stores
each expose the same operation over their own key space --
`JournalSearchKey`, `TimeSeriesSearchKey`,
`MutexSearchKey`, and `CounterSearchKey` --
with identical RE2 semantics
and the same walk-every-key cost under that store's lock.

## The task queue

Tasks are units of work identified by a unique `task_id`.
Each task carries an opaque `function` and `input` payload,
a floating-point `priority`,
and one or more named queues it should be dispatched from.
The set of queues is fixed at `TaskAdd`.
A task moves through the states `Ready` -> `Running` -> `Complete`,
or to `Canceled` from either `Ready` or `Running`.
`Complete` and `Canceled` are both final;
a task in either state never moves again.
A fifth state, `Undefined`, is never held by a live task;
it is what `TaskGetStatus` reports for a `task_id` that does not exist.

| RPC | Description |
| --- | --- |
| `TaskAdd(task_id, queue, priority, function, input)` | Register a new task and enqueue it on each named queue. Returns `ALREADY_EXISTS` if the id is already known. |
| `TaskGet(worker_id, queue)` | Claim the highest-priority `Ready` task from the first queue that has one, mark it `Running` on behalf of `worker_id`, and return its payload. Queues are tried in the order given. Returns `NOT_FOUND` when none of them has work ready. |
| `TaskDone(task_id, output, worker_id)` | Mark a `Running` task `Complete` and store its output. Returns `NOT_FOUND` for an unknown `task_id`, and `FAILED_PRECONDITION` if the task is not `Running` or is held by a different worker. A `Canceled` task is accepted and left alone. |
| `TaskGetStatus(task_id...)` | Return the state of each requested task, in request order. An unknown `task_id` reports `Undefined` rather than being an error. |
| `TaskGetOutput(task_id)` | Return a single task's output. Returns `NOT_FOUND` if the task does not exist; a task that has not completed yet has empty output. |
| `TaskGetCountByState()` | Return how many tasks are currently in each of the `Ready`, `Running`, `Complete`, and `Canceled` states. Takes no arguments. |
| `TaskCancel(task_id)` | Move a `Ready` or `Running` task to `Canceled` and report `success = true`. A task that is already `Complete` or `Canceled` is left alone and reports `success = false`. Returns `NOT_FOUND` for an unknown `task_id`. |
| `TaskGetPriority(task_id)` | Return the task's current priority. Returns `NOT_FOUND` for an unknown `task_id`. |
| `TaskSetPriority(task_id, priority)` | Change the task's priority. A `Ready` task is moved within every queue it is waiting on; a task in any other state records the new priority but is never dispatched again. Returns `NOT_FOUND` for an unknown `task_id`. |
| `TaskGetWorkerId(task_id)` | Return the `worker_id` holding a `Running` task. Returns `NOT_FOUND` for an unknown `task_id`, and `FAILED_PRECONDITION` if the task is not `Running`. |

Within a queue, higher `priority` values are dispatched first,
and tasks of equal priority are dispatched in the order they were added.
A task moved by `TaskSetPriority` counts as newly added at its new priority:
it goes behind the tasks with equal priority already waiting there.

A worker polls using `TaskGet` across the queues it cares about,
runs the work, and reports back with `TaskDone`.

`TaskGet` answers `NOT_FOUND` for a queue with no work ready.

A task belongs to the worker that claimed it,
and `TaskGetWorkerId` reports which one that is.
Only a `Running` task has a holder to report:
a `Ready` task has not been claimed,
a `Complete` one was handed back when it finished,
and cancelling drops the record.
`TaskDone` from any other worker is refused with `FAILED_PRECONDITION`,
so a worker that does not hold the task
cannot overwrite the result of the worker that does.
`TaskDone` on a task that is neither `Running` nor `Canceled`
is refused the same way;
it neither records the output nor reports success.

`TaskCancel` withdraws a task that has not finished.
A `Ready` task is cancelled before anybody claims it,
and a `Running` task is taken away from the worker holding it.
Cancelling never puts a task back on a queue,
and it never touches a task that has already finished:
`success = false` says the task was left exactly as it was.

Cancelling is not automatically communicated to the worker running the task.
The worker may finish the work and call `TaskDone` as usual;
that call succeeds, but the task stays `Canceled`
and the output is discarded.
Reporting cancelled work is not the worker's mistake,
so it is not reported to the worker as an error.
Cancelling also drops the record of which worker held the task,
so `TaskDone` on a `Canceled` task is accepted from whoever sends it --
the ownership rule above has nothing left to check,
and there is no result to overwrite either way.

There is no fault tolerance for a worker that dies mid-task:
the task stays `Running` for as long as the server lives,
and nothing hands it automatically to another worker.
`TaskCancel` only retires such a task.
No RPC returns a task to `Ready`,
and `TaskAdd` refuses a `task_id` that already exists.
So getting the work done will require submitting it again
under a new `task_id`.

## The journal store

A key-to-journal store, where each journal is an append-only,
ordered list of opaque binary entries identified by a `string` key.

| RPC | Description |
| --- | --- |
| `JournalSize(key)` | Return the number of entries in the journal. A journal that does not exist has size `0`. |
| `JournalRead(key, start, end)` | Return the entries in the half-open index range `[start, end)`. |
| `JournalAppend(key, value)` | Append a single entry to the journal, creating it if it does not exist. |
| `JournalSearchKey(pattern)` | Return every journal key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

`JournalRead` uses half-open ranges,
so `JournalRead(key, 0, JournalSize(key))` returns the whole journal.
The range is clamped silently to the journal's bounds:
reading past the end returns only the entries that exist,
and a range with `start >= end` (or a journal that does not exist)
returns an empty list --
neither is an error.

## The time series store

A key-to-series store, where each series is an append-only
list of data points identified by a `string` key.
Each point carries a floating-point `value`, a `datetime`,
and an integer `step`.

| RPC | Description |
| --- | --- |
| `TimeSeriesAppend(key, value, datetime, step)` | Append a point to the series, creating it if it does not exist. `step` is optional and defaults to `0`. Returns `INVALID_ARGUMENT` if `datetime` does not parse. |
| `TimeSeriesGet(key, start_time, end_time, start_step, end_step)` | Return the points of a series, in append order, filtered by the given bounds. A key that does not exist returns an empty list. |
| `TimeSeriesSearchKey(pattern)` | Return every series key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

`datetime` is an ISO 8601 UTC datetime string.
Both the `Z` form (`2024-01-02T03:04:05Z`)
and the offset form produced by Python's `datetime.isoformat()`
(`2024-01-02T03:04:05+00:00`) are accepted,
as is a non-UTC offset (converted to UTC)
or a bare datetime (interpreted as UTC).
Fractional seconds are preserved to microsecond resolution.
Datetimes returned by `TimeSeriesGet` are normalized to the `Z` form.

All four bounds on `TimeSeriesGet` are optional.
`start_time` and `start_step` are inclusive lower bounds;
`end_time` and `end_step` are exclusive upper bounds.
An unset bound imposes no restriction, and the bounds combine:
a point is returned only if it satisfies every bound provided.
Points always come back in the order they were appended,
never sorted by `datetime` or `step`.

## Named mutexes

A map of named locks,
for coordinating exclusive access to a resource across workers.
A mutex is identified by a `string` key
and is either free or held by a `worker_id`.

| RPC | Description |
| --- | --- |
| `MutexTryAcquire(key, worker_id)` | Try once to acquire the mutex, creating it if it does not exist. Returns `true` if this call acquired it, recording `worker_id` as its holder, and `false` if it is already held -- by any worker, `worker_id` included, since the lock is not reentrant. |
| `MutexRelease(key, worker_id)` | Release a mutex held by `worker_id`, clearing the recorded holder. Returns `FAILED_PRECONDITION` if `worker_id` is not the holder, which covers a mutex held by another worker, one that is already free, and one that does not exist. |
| `MutexGetWorkerId(key)` | Return the `worker_id` holding the mutex. Returns `NOT_FOUND` if the key does not exist, and `FAILED_PRECONDITION` if it exists but is free. |
| `MutexSearchKey(pattern)` | Return every mutex key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

A mutex belongs to the `worker_id` that acquired it,
and only that worker can release it.
The lock is not reentrant --
the holder asking again is told the mutex is held, like anybody else.
The `worker_id` is taken at face value,
so these remain **cooperative** locks between workers that agree
on who is called what.

A key exists from the first `MutexTryAcquire` that names it,
whether or not that call acquired it,
and is never removed;
releasing frees the mutex rather than deleting the key.
Neither a refused release nor `MutexGetWorkerId` creates anything.

`MutexGetWorkerId` tells a missing key from a free one:
a key nobody has ever named is `NOT_FOUND`,
while one that exists with no holder is `FAILED_PRECONDITION`.
`MutexRelease` runs the two together,
because to the caller they mean the same thing --
you do not hold this.

Mutexes have no expiry.
A worker that acquires a mutex and then dies leaves it held
for the life of the server.

The Python client adds a blocking
`mutex_acquire(key, worker_id, timeout=None)`
on top of `MutexTryAcquire`.
It retries that call until it succeeds,
sleeping between attempts, and raises `TimeoutError`
if `timeout` seconds elapse first (it retries forever when `timeout` is `None`).

## Counters

A `string -> uint64` map of named counters
that hand out successive integers --
useful for generating unique ids or sequence numbers across workers.

| RPC | Description |
| --- | --- |
| `CounterGetNextValue(key)` | Return the next value of the counter, creating it if it does not exist. The first call for a key returns `1`, and each subsequent call returns the previous value plus one. |
| `CounterGetCurrentValue(key)` | Return the counter's current value without changing it, or `0` if it does not exist. Read-only: it never creates the counter. |
| `CounterSearchKey(pattern)` | Return every counter key matching the regular expression `pattern`. Returns `INVALID_ARGUMENT` if the pattern does not compile. |

`CounterGetNextValue` both creates and advances a counter.
The first `CounterGetNextValue` for a key creates the counter and returns `1`.
`CounterGetCurrentValue` only reads:
it returns the value the last `CounterGetNextValue` handed out
(or `0` for a counter that has never been used) and leaves the counter untouched.
Because counter operations are serialized under the counters' lock,
concurrent callers always receive distinct, gap-free values.
Counters are held in memory only, so a server restart resets every counter --
the next value is `1` again.
