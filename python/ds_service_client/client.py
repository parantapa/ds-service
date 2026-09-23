"""Client for the ds-service server."""

import asyncio
import functools
import os
import random
import time
from collections.abc import Callable, Iterator
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from types import TracebackType

# Both clients call the C++ client in _ext, which hides the transport.
# See "The transport boundary" in docs/developer-notes.md.
from . import _ext
from ._ext import (
    ErrorCode,
    TaskGetCountByStateResponse,
    TaskGetResponse,
    TaskState,
    TimeSeriesDataPoint,
)
from .errors import MutexNotHeld, NoTaskAvailable, TaskStateError, TransportError

# Default deadline applied to every call, in seconds.
# Without one, a server that accepts the connection
# but never answers hangs the caller forever.
DEFAULT_RPC_TIMEOUT_S = 5 * 60.0

# Base sleep, and its +/- jitter, between mutex_acquire retries, in seconds.
# The jitter keeps workers that contend for one mutex
# from retrying in lockstep.
# cpp/client/client.cpp uses the same values for the C++ mutex_acquire.
MUTEX_ACQUIRE_SLEEP_S = 0.5
MUTEX_ACQUIRE_JITTER_S = 0.1

# A child forked after a client was created refuses every call,
# because a call there would hang.
# See "A client does not survive fork()" in docs/developer-notes.md.
FORK_MESSAGE = (
    "ds_service_client cannot make calls in a process forked "
    "after a client was created, because its gRPC does not survive fork(). "
    "Use the spawn or forkserver start method of multiprocessing, "
    "or create the first client after forking."
)
_client_created = False
_forked_after_client = False


def _after_fork_in_child() -> None:
    global _forked_after_client
    if _client_created:
        _forked_after_client = True


os.register_at_fork(after_in_child=_after_fork_in_child)


@contextmanager
def translate_error(
    not_found: type[Exception] = KeyError,
    failed_precondition: type[Exception] = TaskStateError,
) -> Iterator[None]:
    """Re-raise a failed call as the exception this client documents.

    `not_found` overrides what ErrorCode.NotFound maps to,
    because the code means "no such key" on most calls
    but "no task is ready" on task_get.
    `failed_precondition` does the same for the code
    the server uses to refuse an operation on state or ownership grounds.
    That code covers four cases:

    * A task that is not Running, or is held by another worker, on task_done.
    * A task that is not Running on task_get_worker_id.
    * A mutex that does not exist, is free, or is held by another worker,
      on mutex_release.
    * A mutex that is free on mutex_get_worker_id.

    A call on a closed client raises RuntimeError.
    So does every call in a process forked after a client was created.
    See FORK_MESSAGE.
    A failure with no documented mapping raises TransportError.
    """
    # Every call to the server goes through here,
    # so this one check covers them all.
    # close() does not, and it makes no call to the server.
    if _forked_after_client:
        raise RuntimeError(FORK_MESSAGE)
    try:
        yield
    except _ext.ClientError as e:
        code, message = e.args
        match code:
            case ErrorCode.NotFound:
                raise not_found(message) from e
            case (
                ErrorCode.AlreadyExists
                | ErrorCode.InvalidArgument
                | ErrorCode.MessageTooLarge
            ):
                # MessageTooLarge is a message larger than the transport accepts,
                # that is, a caller-side size problem,
                # so it reads as a ValueError.
                raise ValueError(message) from e
            case ErrorCode.FailedPrecondition:
                raise failed_precondition(message) from e
            case ErrorCode.Unavailable | ErrorCode.DeadlineExceeded:
                raise TimeoutError(message) from e
            case ErrorCode.Closed:
                raise RuntimeError(message) from e
            case _:
                raise TransportError(message) from e


def as_queue_list(queue: str | list[str]) -> list[str]:
    """Return the queue names of a queue argument, whether one or several."""
    if isinstance(queue, str):
        return [queue]
    return queue


def as_parent_task_id_list(parent_task_ids: str | list[str] | None) -> list[str]:
    """Return the parent ids of a parent_task_ids argument.

    None and an empty list are both no ids at all.
    """
    if parent_task_ids is None:
        return []
    if isinstance(parent_task_ids, str):
        return [parent_task_ids]
    return parent_task_ids


def mutex_retry_delay(key: str, deadline: float | None) -> float:
    """Return how long to wait before the next mutex_try_acquire attempt.

    deadline is a time.monotonic() reading, or None for "retry forever".
    Raise TimeoutError once the deadline passes.
    Otherwise, shorten the delay
    so that the wait does not overshoot it.
    """
    delay = MUTEX_ACQUIRE_SLEEP_S + random.uniform(
        -MUTEX_ACQUIRE_JITTER_S, MUTEX_ACQUIRE_JITTER_S
    )
    if deadline is None:
        return delay

    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError(f"Timed out acquiring mutex {key!r}.")
    return min(delay, remaining)


def connect(address: str | None, timeout: float) -> tuple[str, _ext.Client]:
    """Resolve the address, and connect to it.

    address None selects the DS_SERVER_ADDRESS environment variable.
    Raise KeyError when neither is set,
    and ValueError for an empty address or one that names no transport.
    The connection is made lazily,
    so an unreachable server fails the first call rather than this one.
    """
    global _client_created
    if address is None:
        address = os.environ["DS_SERVER_ADDRESS"]
    with translate_error():
        client = _ext.connect(address, timeout)
    # Arms the fork guard in _after_fork_in_child. See FORK_MESSAGE.
    _client_created = True
    return address, client


class DsServiceClient:
    """A connection to a ds-service server, and the calls it offers.

    address is host:port, or grpc://host:port.
    It defaults to the DS_SERVER_ADDRESS environment variable,
    and the constructor raises KeyError when neither is set,
    and ValueError for an empty address or one that names no transport.
    The client applies timeout, in seconds, as the deadline of every call.
    The methods are safe to call from several threads at once.
    """

    def __init__(
        self,
        address: str | None = None,
        timeout: float = DEFAULT_RPC_TIMEOUT_S,
    ) -> None:
        self.address, self.client = connect(address, timeout)
        self.timeout = timeout

    def close(self) -> None:
        """Close the client.

        Calls in flight, and every later call, raise RuntimeError.
        Safe to call more than once.
        """
        self.client.close()

    def __enter__(self) -> "DsServiceClient":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()

    def map_set(self, key: str, value: bytes) -> None:
        """Store value under key.

        The new value replaces any value already there.
        """
        with translate_error():
            self.client.map_set(key, value)

    def map_get(self, key: str) -> bytes:
        """Return the value stored under key.

        Raise KeyError if the key does not exist.
        """
        with translate_error():
            return self.client.map_get(key)

    def map_search_key(self, pattern: str) -> list[str]:
        """Return the map keys matching the RE2 regular expression pattern.

        The match is unanchored, so it succeeds on any substring of a key
        unless the pattern anchors itself with ^ and $.
        The server returns the keys in unspecified order.
        Raise ValueError if the pattern does not compile.
        """
        with translate_error():
            return self.client.map_search_key(pattern)

    def task_add(
        self,
        task_id: str,
        parent_task_ids: str | list[str],
        queue: str | list[str],
        priority: float,
        function: bytes,
        input: bytes,
    ) -> None:
        """Register a task, and enqueue it on each of its queues once it is Ready.

        parent_task_ids names the tasks this one depends on,
        as one id or a list of them.
        An empty list means the task has no parents.
        A task with a parent that is not Finished starts TaskState.Waiting,
        unless a parent is Canceled or Failed,
        and no queue dispatches it until every parent finishes.
        Every parent must already exist,
        so a graph of tasks is added parents first.
        Raise KeyError for a parent the server does not know,
        and add nothing in that case.
        A task added with a Canceled parent is added Canceled,
        and one added with a Failed parent is added Failed.
        A task with both is added Failed.

        queue is one queue name or a list of them,
        and the set is fixed for the life of the task.
        function and input are opaque payloads the server only stores.
        Raise ValueError if task_id is already known.
        """
        with translate_error():
            self.client.task_add(
                task_id,
                as_parent_task_id_list(parent_task_ids),
                as_queue_list(queue),
                priority,
                function,
                input,
            )

    def task_get_status(self, task_id: str | list[str]) -> TaskState | list[TaskState]:
        """Return the state of one task, or of each task in a list.

        A single string returns a single TaskState.
        A list returns a list of them, one per id in the same order.
        An id the server does not know reports TaskState.Undefined
        rather than raising.
        """
        single = isinstance(task_id, str)
        task_ids = [task_id] if single else task_id

        with translate_error():
            states = self.client.task_get_status(task_ids)
        return states[0] if single else states

    def task_get_output(self, task_id: str) -> bytes:
        """Return the output recorded for a task.

        A task that has not ended has empty output.
        A canceled task reports b"Task canceled",
        and a task failed by one it depends on reports
        b"Dependency failed (task_id=...)", naming the task whose run failed.
        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            return self.client.task_get_output(task_id)

    def task_get_count_by_state(self) -> TaskGetCountByStateResponse:
        """Return how many tasks are in each state.

        The counts sum to every task the server knows about.
        """
        with translate_error():
            return self.client.task_get_count_by_state()

    def task_get_priority(self, task_id: str) -> float:
        """Return the current priority of an existing task.

        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            return self.client.task_get_priority(task_id)

    def task_set_priority(self, task_id: str, priority: float) -> None:
        """Change the priority of an existing task.

        A Ready task goes behind the tasks of equal priority already waiting.
        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            self.client.task_set_priority(task_id, priority)

    def task_cancel(self, task_id: str) -> bool:
        """Move a Waiting, Ready or Running task to Canceled.

        Return True if this call moved the task,
        and False if it was already Finished, Failed or Canceled
        and so was left alone.
        Canceling a task cancels every task waiting on it,
        and every task waiting on those.
        Each task canceled reports b"Task canceled" as its output.
        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            return self.client.task_cancel(task_id)

    def task_get_worker_id(self, task_id: str) -> str:
        """Return the worker holding a Running task.

        Raise KeyError for a task_id the server does not know,
        and TaskStateError if the task is not Running.
        """
        with translate_error():
            return self.client.task_get_worker_id(task_id)

    def task_search_id(self, pattern: str) -> list[str]:
        """Return the task ids matching the RE2 pattern.

        Same semantics as map_search_key, over the task ids.
        The server searches tasks in every state.
        """
        with translate_error():
            return self.client.task_search_id(pattern)

    def task_get(self, worker_id: str, queue: str | list[str]) -> TaskGetResponse:
        """Claim a task for worker_id from the first queue holding one.

        The server tries the queues in the order given.
        From that queue it takes the task with the highest priority,
        and tasks of equal priority in the order they entered the queue.
        Raise NoTaskAvailable, not TimeoutError,
        when none of them has a task ready.
        """
        # A distinct exception keeps idle work
        # distinguishable from an unreachable server.
        with translate_error(not_found=NoTaskAvailable):
            return self.client.task_get(worker_id, as_queue_list(queue))

    def task_done(
        self, task_id: str, worker_id: str, output: bytes, failed: bool = False
    ) -> None:
        """Record a task's output and mark it Finished, or Failed.

        worker_id must be the one that claimed the task through task_get.
        Raise KeyError for a task_id the server does not know,
        and TaskStateError if the task is not Running,
        or if it is held by a different worker.

        failed says how the task ended.
        The default marks it TaskState.Finished,
        and failed=True marks it TaskState.Failed.
        The server stores output either way.
        Failing a task fails every task waiting on it,
        and every task waiting on those,
        and each of them reports b"Dependency failed (task_id=...)"
        as its output, naming this task.

        A canceled task is the exception:
        the call succeeds whatever worker_id it names,
        but the task stays Canceled
        and the server discards the output.
        """
        with translate_error():
            self.client.task_done(task_id, worker_id, output, failed)

    def journal_size(self, key: str) -> int:
        """Return the number of entries in a journal.

        A journal that does not exist has size 0.
        """
        with translate_error():
            return self.client.journal_size(key)

    def journal_read(self, key: str, start: int, end: int) -> list[bytes]:
        """Return the entries in the half-open index range [start, end).

        The range is clamped to the journal's bounds,
        so a read past the end returns only the entries that exist.
        An empty range returns an empty list rather than raising.
        A journal that does not exist also returns an empty list.
        """
        with translate_error():
            return self.client.journal_read(key, start, end)

    def journal_append(self, key: str, value: bytes) -> None:
        """Append one entry to a journal.

        The server creates the journal if it does not exist.
        """
        with translate_error():
            self.client.journal_append(key, value)

    def journal_search_key(self, pattern: str) -> list[str]:
        """Return the journal keys matching the RE2 pattern.

        Same semantics as map_search_key, over the journal key space.
        """
        with translate_error():
            return self.client.journal_search_key(pattern)

    def time_series_append(
        self, key: str, value: float, datetime: str, step: int = 0
    ) -> None:
        """Append a point to a series.

        The server creates the series if it does not exist.
        datetime is an ISO 8601 UTC string.
        The server accepts the Z form, an offset form, and a bare datetime.
        Raise ValueError if it does not parse.
        """
        with translate_error():
            self.client.time_series_append(key, value, datetime, step)

    def time_series_get(
        self,
        key: str,
        start_time: str | None = None,
        end_time: str | None = None,
        start_step: int | None = None,
        end_step: int | None = None,
    ) -> list[TimeSeriesDataPoint]:
        """Return the points of a series that satisfy every bound given.

        start_time and start_step are inclusive,
        end_time and end_step exclusive,
        and a bound left as None imposes no restriction.
        An empty string as start_time or end_time imposes none either.
        The server returns the points in the order the caller appended them.
        A key that does not exist returns an empty list.
        Raise ValueError if start_time or end_time does not parse.
        """
        with translate_error():
            return self.client.time_series_get(
                key, start_time, end_time, start_step, end_step
            )

    def time_series_search_key(self, pattern: str) -> list[str]:
        """Return the series keys matching the RE2 pattern.

        Same semantics as map_search_key, over the keys of the time series.
        """
        with translate_error():
            return self.client.time_series_search_key(pattern)

    def mutex_try_acquire(self, key: str, worker_id: str) -> bool:
        """Try once to acquire a mutex on behalf of worker_id.

        Return True if this call acquired it,
        which records worker_id as its holder.
        Return False if the mutex is already held,
        which includes worker_id holding it already:
        the lock is not reentrant.
        """
        with translate_error():
            return self.client.mutex_try_acquire(key, worker_id)

    def mutex_release(self, key: str, worker_id: str) -> None:
        """Release a mutex held by worker_id.

        Raise MutexNotHeld if worker_id is not its holder,
        which includes a mutex that is already free
        and one that does not exist.
        """
        with translate_error(failed_precondition=MutexNotHeld):
            self.client.mutex_release(key, worker_id)

    def mutex_get_worker_id(self, key: str) -> str:
        """Return the worker holding the mutex.

        Raise KeyError if the mutex does not exist,
        and MutexNotHeld if it exists but is free.
        The mutex does not exist until a mutex_try_acquire call names the key.
        """
        with translate_error(failed_precondition=MutexNotHeld):
            return self.client.mutex_get_worker_id(key)

    def mutex_search_key(self, pattern: str) -> list[str]:
        """Return the mutex keys matching the RE2 pattern.

        Same semantics as map_search_key, over the mutex key space.
        A key exists from the first mutex_try_acquire that names it,
        whether or not that call acquired it.
        A free mutex is listed like a held one.
        """
        with translate_error():
            return self.client.mutex_search_key(pattern)

    def mutex_acquire(
        self, key: str, worker_id: str, timeout: float | None = None
    ) -> None:
        """Block until worker_id acquires the mutex.

        Raise TimeoutError once timeout seconds elapse.
        With timeout None, retry forever.
        The timeout bounds the whole loop, sleeps included.
        """
        # The server offers no blocking acquire,
        # so this loop polls mutex_try_acquire.
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if self.mutex_try_acquire(key, worker_id):
                return

            delay = mutex_retry_delay(key, deadline)
            time.sleep(delay)

    def counter_get_next_value(self, key: str) -> int:
        """Advance a counter and return its new value.

        The first call for a key creates the counter and returns 1.
        Each later call returns the previous value plus one.
        Concurrent callers receive distinct, gap-free values.
        """
        with translate_error():
            return self.client.counter_get_next_value(key)

    def counter_get_current_value(self, key: str) -> int:
        """Return a counter's current value without advancing it.

        A counter that does not exist reads as 0 and is not created.
        """
        with translate_error():
            return self.client.counter_get_current_value(key)

    def counter_search_key(self, pattern: str) -> list[str]:
        """Return the counter keys matching the RE2 pattern.

        Same semantics as map_search_key, over the counter key space.
        """
        with translate_error():
            return self.client.counter_search_key(pattern)


# Each method mirrors one on DsServiceClient,
# and tests/test_client_parity.py fails when the two drift apart.
# See "Two clients, one API" in docs/developer-notes.md.
class DsServiceClientAsync:
    """An asyncio connection to a ds-service server, and the calls it offers.

    This class offers the same API as DsServiceClient.
    Each method is a coroutine, and `async with` replaces `with`.
    The exceptions each method documents, and the meaning of every argument,
    are unchanged.

    Each call runs on a thread of the client's own pool
    while the event loop goes on with other work.
    max_workers caps how many calls run at once.
    None takes the default of concurrent.futures.ThreadPoolExecutor,
    which grows with the number of CPUs.
    Canceling the coroutine does not cancel the call itself,
    which runs on until it completes or its deadline passes.
    """

    def __init__(
        self,
        address: str | None = None,
        timeout: float = DEFAULT_RPC_TIMEOUT_S,
        max_workers: int | None = None,
    ) -> None:
        self.address, self.client = connect(address, timeout)
        self.timeout = timeout
        self._executor = ThreadPoolExecutor(
            max_workers=max_workers, thread_name_prefix="ds-service-client"
        )
        self._closed = False

    async def _call[**P, R](
        self, method: Callable[P, R], *args: P.args, **kwargs: P.kwargs
    ) -> R:
        """Run one blocking call of self.client on the pool, and await it."""
        if self._closed:
            # A closed client raises at once without blocking,
            # and the pool accepts no more work.
            return method(*args, **kwargs)
        # See "Two clients, one API" in docs/developer-notes.md
        # for why a thread pool is enough to run calls concurrently.
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            self._executor, functools.partial(method, *args, **kwargs)
        )

    async def close(self) -> None:
        """Close the client, and release its threads.

        Calls in flight, and every later call, raise RuntimeError.
        Safe to call more than once.
        """
        self._closed = True
        self.client.close()
        # A wait here would block the event loop.
        # The calls still in flight fail on their own, now that the client is closed.
        self._executor.shutdown(wait=False)

    async def __aenter__(self) -> "DsServiceClientAsync":
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        await self.close()

    async def map_set(self, key: str, value: bytes) -> None:
        """Store value under key.

        The new value replaces any value already there.
        """
        with translate_error():
            await self._call(self.client.map_set, key, value)

    async def map_get(self, key: str) -> bytes:
        """Return the value stored under key.

        Raise KeyError if the key does not exist.
        """
        with translate_error():
            return await self._call(self.client.map_get, key)

    async def map_search_key(self, pattern: str) -> list[str]:
        """Return the map keys matching the RE2 regular expression pattern.

        The match is unanchored, so it succeeds on any substring of a key
        unless the pattern anchors itself with ^ and $.
        The server returns the keys in unspecified order.
        Raise ValueError if the pattern does not compile.
        """
        with translate_error():
            return await self._call(self.client.map_search_key, pattern)

    async def task_add(
        self,
        task_id: str,
        parent_task_ids: str | list[str],
        queue: str | list[str],
        priority: float,
        function: bytes,
        input: bytes,
    ) -> None:
        """Register a task, and enqueue it on each of its queues once it is Ready.

        parent_task_ids names the tasks this one depends on,
        as one id or a list of them.
        An empty list means the task has no parents.
        A task with a parent that is not Finished starts TaskState.Waiting,
        unless a parent is Canceled or Failed,
        and no queue dispatches it until every parent finishes.
        Every parent must already exist,
        so a graph of tasks is added parents first.
        Raise KeyError for a parent the server does not know,
        and add nothing in that case.
        A task added with a Canceled parent is added Canceled,
        and one added with a Failed parent is added Failed.
        A task with both is added Failed.

        queue is one queue name or a list of them,
        and the set is fixed for the life of the task.
        function and input are opaque payloads the server only stores.
        Raise ValueError if task_id is already known.
        """
        with translate_error():
            await self._call(
                self.client.task_add,
                task_id,
                as_parent_task_id_list(parent_task_ids),
                as_queue_list(queue),
                priority,
                function,
                input,
            )

    async def task_get_status(
        self, task_id: str | list[str]
    ) -> TaskState | list[TaskState]:
        """Return the state of one task, or of each task in a list.

        A single string returns a single TaskState.
        A list returns a list of them, one per id in the same order.
        An id the server does not know reports TaskState.Undefined
        rather than raising.
        """
        single = isinstance(task_id, str)
        task_ids = [task_id] if single else task_id

        with translate_error():
            states = await self._call(self.client.task_get_status, task_ids)
        return states[0] if single else states

    async def task_get_output(self, task_id: str) -> bytes:
        """Return the output recorded for a task.

        A task that has not ended has empty output.
        A canceled task reports b"Task canceled",
        and a task failed by one it depends on reports
        b"Dependency failed (task_id=...)", naming the task whose run failed.
        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            return await self._call(self.client.task_get_output, task_id)

    async def task_get_count_by_state(self) -> TaskGetCountByStateResponse:
        """Return how many tasks are in each state.

        The counts sum to every task the server knows about.
        """
        with translate_error():
            return await self._call(self.client.task_get_count_by_state)

    async def task_get_priority(self, task_id: str) -> float:
        """Return the current priority of an existing task.

        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            return await self._call(self.client.task_get_priority, task_id)

    async def task_set_priority(self, task_id: str, priority: float) -> None:
        """Change the priority of an existing task.

        A Ready task goes behind the tasks of equal priority already waiting.
        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            await self._call(self.client.task_set_priority, task_id, priority)

    async def task_cancel(self, task_id: str) -> bool:
        """Move a Waiting, Ready or Running task to Canceled.

        Return True if this call moved the task,
        and False if it was already Finished, Failed or Canceled
        and so was left alone.
        Canceling a task cancels every task waiting on it,
        and every task waiting on those.
        Each task canceled reports b"Task canceled" as its output.
        Raise KeyError for a task_id the server does not know.
        """
        with translate_error():
            return await self._call(self.client.task_cancel, task_id)

    async def task_get_worker_id(self, task_id: str) -> str:
        """Return the worker holding a Running task.

        Raise KeyError for a task_id the server does not know,
        and TaskStateError if the task is not Running.
        """
        with translate_error():
            return await self._call(self.client.task_get_worker_id, task_id)

    async def task_search_id(self, pattern: str) -> list[str]:
        """Return the task ids matching the RE2 pattern.

        Same semantics as map_search_key, over the task ids.
        The server searches tasks in every state.
        """
        with translate_error():
            return await self._call(self.client.task_search_id, pattern)

    async def task_get(self, worker_id: str, queue: str | list[str]) -> TaskGetResponse:
        """Claim a task for worker_id from the first queue holding one.

        The server tries the queues in the order given.
        From that queue it takes the task with the highest priority,
        and tasks of equal priority in the order they entered the queue.
        Raise NoTaskAvailable, not TimeoutError,
        when none of them has a task ready.
        """
        # A distinct exception keeps idle work
        # distinguishable from an unreachable server.
        with translate_error(not_found=NoTaskAvailable):
            return await self._call(
                self.client.task_get, worker_id, as_queue_list(queue)
            )

    async def task_done(
        self, task_id: str, worker_id: str, output: bytes, failed: bool = False
    ) -> None:
        """Record a task's output and mark it Finished, or Failed.

        worker_id must be the one that claimed the task through task_get.
        Raise KeyError for a task_id the server does not know,
        and TaskStateError if the task is not Running,
        or if it is held by a different worker.

        failed says how the task ended.
        The default marks it TaskState.Finished,
        and failed=True marks it TaskState.Failed.
        The server stores output either way.
        Failing a task fails every task waiting on it,
        and every task waiting on those,
        and each of them reports b"Dependency failed (task_id=...)"
        as its output, naming this task.

        A canceled task is the exception:
        the call succeeds whatever worker_id it names,
        but the task stays Canceled
        and the server discards the output.
        """
        with translate_error():
            await self._call(self.client.task_done, task_id, worker_id, output, failed)

    async def journal_size(self, key: str) -> int:
        """Return the number of entries in a journal.

        A journal that does not exist has size 0.
        """
        with translate_error():
            return await self._call(self.client.journal_size, key)

    async def journal_read(self, key: str, start: int, end: int) -> list[bytes]:
        """Return the entries in the half-open index range [start, end).

        The range is clamped to the journal's bounds,
        so a read past the end returns only the entries that exist.
        An empty range returns an empty list rather than raising.
        A journal that does not exist also returns an empty list.
        """
        with translate_error():
            return await self._call(self.client.journal_read, key, start, end)

    async def journal_append(self, key: str, value: bytes) -> None:
        """Append one entry to a journal.

        The server creates the journal if it does not exist.
        """
        with translate_error():
            await self._call(self.client.journal_append, key, value)

    async def journal_search_key(self, pattern: str) -> list[str]:
        """Return the journal keys matching the RE2 pattern.

        Same semantics as map_search_key, over the journal key space.
        """
        with translate_error():
            return await self._call(self.client.journal_search_key, pattern)

    async def time_series_append(
        self, key: str, value: float, datetime: str, step: int = 0
    ) -> None:
        """Append a point to a series.

        The server creates the series if it does not exist.
        datetime is an ISO 8601 UTC string.
        The server accepts the Z form, an offset form, and a bare datetime.
        Raise ValueError if it does not parse.
        """
        with translate_error():
            await self._call(self.client.time_series_append, key, value, datetime, step)

    async def time_series_get(
        self,
        key: str,
        start_time: str | None = None,
        end_time: str | None = None,
        start_step: int | None = None,
        end_step: int | None = None,
    ) -> list[TimeSeriesDataPoint]:
        """Return the points of a series that satisfy every bound given.

        start_time and start_step are inclusive,
        end_time and end_step exclusive,
        and a bound left as None imposes no restriction.
        An empty string as start_time or end_time imposes none either.
        The server returns the points in the order the caller appended them.
        A key that does not exist returns an empty list.
        Raise ValueError if start_time or end_time does not parse.
        """
        with translate_error():
            return await self._call(
                self.client.time_series_get,
                key,
                start_time,
                end_time,
                start_step,
                end_step,
            )

    async def time_series_search_key(self, pattern: str) -> list[str]:
        """Return the series keys matching the RE2 pattern.

        Same semantics as map_search_key, over the keys of the time series.
        """
        with translate_error():
            return await self._call(self.client.time_series_search_key, pattern)

    async def mutex_try_acquire(self, key: str, worker_id: str) -> bool:
        """Try once to acquire a mutex on behalf of worker_id.

        Return True if this call acquired it,
        which records worker_id as its holder.
        Return False if the mutex is already held,
        which includes worker_id holding it already:
        the lock is not reentrant.
        """
        with translate_error():
            return await self._call(self.client.mutex_try_acquire, key, worker_id)

    async def mutex_release(self, key: str, worker_id: str) -> None:
        """Release a mutex held by worker_id.

        Raise MutexNotHeld if worker_id is not its holder,
        which includes a mutex that is already free
        and one that does not exist.
        """
        with translate_error(failed_precondition=MutexNotHeld):
            await self._call(self.client.mutex_release, key, worker_id)

    async def mutex_get_worker_id(self, key: str) -> str:
        """Return the worker holding the mutex.

        Raise KeyError if the mutex does not exist,
        and MutexNotHeld if it exists but is free.
        The mutex does not exist until a mutex_try_acquire call names the key.
        """
        with translate_error(failed_precondition=MutexNotHeld):
            return await self._call(self.client.mutex_get_worker_id, key)

    async def mutex_search_key(self, pattern: str) -> list[str]:
        """Return the mutex keys matching the RE2 pattern.

        Same semantics as map_search_key, over the mutex key space.
        A key exists from the first mutex_try_acquire that names it,
        whether or not that call acquired it.
        A free mutex is listed like a held one.
        """
        with translate_error():
            return await self._call(self.client.mutex_search_key, pattern)

    async def mutex_acquire(
        self, key: str, worker_id: str, timeout: float | None = None
    ) -> None:
        """Wait until worker_id acquires the mutex.

        Raise TimeoutError once timeout seconds elapse.
        With timeout None, retry forever.
        The timeout bounds the whole loop, sleeps included.
        Only this coroutine waits:
        the sleeps yield to the event loop.
        """
        # The server offers no blocking acquire,
        # so this loop polls mutex_try_acquire.
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if await self.mutex_try_acquire(key, worker_id):
                return

            delay = mutex_retry_delay(key, deadline)
            await asyncio.sleep(delay)

    async def counter_get_next_value(self, key: str) -> int:
        """Advance a counter and return its new value.

        The first call for a key creates the counter and returns 1.
        Each later call returns the previous value plus one.
        Concurrent callers receive distinct, gap-free values.
        """
        with translate_error():
            return await self._call(self.client.counter_get_next_value, key)

    async def counter_get_current_value(self, key: str) -> int:
        """Return a counter's current value without advancing it.

        A counter that does not exist reads as 0 and is not created.
        """
        with translate_error():
            return await self._call(self.client.counter_get_current_value, key)

    async def counter_search_key(self, pattern: str) -> list[str]:
        """Return the counter keys matching the RE2 pattern.

        Same semantics as map_search_key, over the counter key space.
        """
        with translate_error():
            return await self._call(self.client.counter_search_key, pattern)
