# About the task queue

Why the task queue behaves as it does,
and what it leaves to the caller.

For the RPCs themselves, see
the [data structure reference](data-structure-reference.md#the-task-queue).

## A task belongs to the worker that claimed it

`TaskGet` records the `worker_id` that claimed a task,
and the server refuses `TaskDone` from any other worker.
The point is that a worker which does not hold a task
cannot overwrite the result of the worker that does.

The server takes the `worker_id` at face value.
Two processes that call themselves the same thing are the same worker
as far as the server is concerned.
Ownership here is a bookkeeping device that keeps honest workers
from overwriting each other's results,
not an authentication mechanism.

## Canceling does not reach the worker

`TaskCancel` moves a task to `Canceled`,
but nothing tells the worker that runs it.
The worker can finish the work and call `TaskDone` as usual.
That call succeeds, and the server discards the output.

The alternative was to refuse the call.
`ds-service` rejects that alternative,
because reporting canceled work is not the worker's mistake.
A worker that did its job correctly
must not handle an error for an event it did not cause.

Canceling also drops the record of which worker held the task.
The server accepts `TaskDone` on a canceled task from anybody,
because it returns on the canceled state before it checks ownership.
The ownership rule has nothing left to check,
and there is no result to overwrite either way.

## There is no fault tolerance

A worker that dies mid-task leaves that task `Running`
for as long as the server lives.
Nothing detects the death, and nothing hands the work to another worker.
No RPC returns a task to `Ready`,
and `TaskAdd` refuses a `task_id` that already exists.
So you resubmit the work under a new id, or not at all.

The server has no liveness signal.
The server never hears from a worker between `TaskGet` and `TaskDone`.
So it cannot tell a dead worker from a slow one.
A lease long enough not to steal work from slow workers
is too long to be useful for recovery.
Rather than guess, the server does nothing,
and leaves the policy to a caller that knows how long its own tasks take.

`TaskCancel` is the tool for retiring such a task.

## The server reclaims nothing

A task keeps its row for the life of the server process,
even after it reaches `Complete` or `Canceled`.
A long-lived server therefore accumulates rows
in proportion to the total number of tasks ever added,
rather than the number currently outstanding.
`TaskSearchId` walks all of them.

Queue entries accumulate the same way.
`TaskSetPriority` is the reliable way to produce them.
A raised priority leaves a dead entry at the old value
that a busy queue never pops.

Both are consequences of addressing a task row by its index,
and both are limitations, not design.
The developer notes explain what compaction costs.
