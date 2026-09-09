# About the task queue

Why the task queue behaves as it does,
and what it leaves to the caller.

For the RPCs themselves, see the
[data structure reference](data-structure-reference.md#the-task-queue).

## A task belongs to the worker that claimed it

`TaskGet` records the `worker_id` that claimed a task,
and `TaskDone` from any other worker is refused.
The point is that a worker which does not hold a task
cannot overwrite the result of the worker that does.

The `worker_id` is taken at face value.
Two processes that call themselves the same thing are the same worker
as far as the server is concerned.
Ownership here is a bookkeeping device that keeps honest workers
from stepping on each other,
not an authentication mechanism.

## Cancelling does not reach the worker

`TaskCancel` moves a task to `Canceled`,
but nothing tells the worker that is running it.
The worker may finish the work and call `TaskDone` as usual.
That call succeeds and the output is discarded.

Refusing the call instead was the alternative,
and it was rejected because reporting cancelled work
is not the worker's mistake.
A worker that did its job correctly
should not have to handle an error for something
that happened behind its back.

Cancelling also drops the record of which worker held the task,
which is why `TaskDone` on a cancelled task is accepted from anybody:
the ownership rule has nothing left to check,
and there is no result to overwrite either way.

## There is no fault tolerance

A worker that dies mid-task leaves that task `Running`
for as long as the server lives.
Nothing detects the death, and nothing hands the work to another worker.
No RPC returns a task to `Ready`,
and `TaskAdd` refuses a `task_id` that already exists,
so the work is resubmitted under a new id or not at all.

This follows from the absence of any liveness signal.
The server never hears from a worker between `TaskGet` and `TaskDone`,
so it cannot tell a dead worker from a slow one,
and a lease long enough not to steal work from slow workers
is too long to be useful for recovery.
Rather than guess, the server does nothing,
and leaves the policy to a caller that actually knows
how long its own tasks should take.

`TaskCancel` is the tool for retiring such a task.

## Nothing is reclaimed

A task keeps its row for the life of the server process,
including after it reaches `Complete` or `Canceled`.
A long-lived server therefore accumulates rows
in proportion to the total number of tasks ever added,
rather than the number currently outstanding,
and `TaskSearchId` walks all of them.

Queue entries accumulate the same way.
`TaskSetPriority` is the reliable way to produce them:
raising a task's priority leaves a dead entry at the old value
that a busy queue may never pop.

Both are consequences of addressing a task row by its index,
and both are documented as limitations rather than as design;
the developer notes explain what compacting them would cost.
