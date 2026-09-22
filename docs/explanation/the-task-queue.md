# About the task queue

Why the task queue behaves as it does,
and what it leaves to the caller.

For the RPCs themselves, see
the [data structure reference](../reference/data-structure.md#task-queue).

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

## A task that ends badly takes its dependents with it

`TaskCancel` cancels every task waiting on the task it cancels,
and every task waiting on those.
`TaskDone` with `failed` does the same, as a failure.
`TaskAdd` does it in advance:
a task named with a `Canceled` parent is added `Canceled`,
and one named with a `Failed` parent is added `Failed`.

All of it follows from one fact.
A task is released when its last parent reaches `Finished`,
and neither a canceled nor a failed task ever reaches it.
Leaving the dependents `Waiting` would leave them waiting for an event
that cannot happen,
so they would sit in the `waiting` count for the life of the server
and never reach a state a caller can act on.

The server ends them instead of refusing the cancellation or the failure,
because the caller that withdraws a task,
and the worker that reports one it could not do,
are both saying the work that task stands for did not happen.
A task that exists only to consume that work has nothing left to do.

`TaskGetOutput` is where a dependent says so.
A task canceled, whether by name or through a parent,
reports `Task canceled`.
A task failed through a parent reports
`Dependency failed (task_id=...)`,
naming the task whose own run failed
rather than the parent it was waiting on.
A chain of them therefore points at the one task to look at,
however far down the chain the reader starts.
A task that ran keeps whatever its worker reported,
which is how a failing worker passes a traceback on to whoever asks.

The state a dependent inherits is the parent's, not a state of its own.
A caller counting `failed` tasks therefore counts the work that failed
together with the work that could not be attempted,
and `TaskGetOutput` is what tells the two apart.

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
even after it reaches `Finished`, `Failed` or `Canceled`.
The row holds the ids of the tasks waiting on it as well,
so a dependency graph is kept whole
long after every task in it has finished.
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
The [developer notes](../developer-notes.md#known-limitations)
explain what compaction costs.
