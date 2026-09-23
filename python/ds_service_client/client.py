"""Client for the ds-service server."""

import asyncio
import os
import random
import time
from collections.abc import Iterator
from contextlib import contextmanager
from types import TracebackType

# These star imports also supply grpc itself, and TaskState,
# which the package __init__ re-exports from here.
from .ds_service_pb2 import *
from .ds_service_pb2_grpc import *

# Largest single request or response accepted, in bytes.
# Must match MAX_MESSAGE_SIZE_BYTES in cpp/grpc/channel-settings.hpp.
# gRPC's own default is 4 MiB.
# See "The channel settings are one setting in two languages"
# in docs/developer-notes.md.
MAX_MESSAGE_SIZE_BYTES = 64 * 1024 * 1024

# Several of these options must match the server's channel settings
# in cpp/grpc/channel-settings.hpp.
# See "The channel settings are one setting in two languages"
# in docs/developer-notes.md.
GRPC_CLIENT_OPTIONS: list[tuple[str, int]] = [
    # This ping interval must stay above the server's
    # SERVER_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS
    # (10 seconds in cpp/grpc/channel-settings.hpp).
    ("grpc.keepalive_time_ms", 120 * 1000),
    ("grpc.keepalive_timeout_ms", 30 * 1000),
    # 0 means "unlimited".
    # The cap is a total of pings sent while no RPC is in flight, not a rate,
    # so any finite value stops the pings on a long-idle connection,
    # the one connection keepalive_permit_without_calls protects.
    ("grpc.http2.max_pings_without_data", 0),
    ("grpc.keepalive_permit_without_calls", 1),
    ("grpc.max_receive_message_length", MAX_MESSAGE_SIZE_BYTES),
    ("grpc.max_send_message_length", MAX_MESSAGE_SIZE_BYTES),
]

# Default deadline applied to every RPC, in seconds.
# Without one, a server that accepts the connection
# but never answers hangs the caller forever.
DEFAULT_RPC_TIMEOUT_S = 5 * 60.0

# Base sleep, and its +/- jitter, between mutex_acquire retries, in seconds.
# The jitter keeps workers that contend for one mutex
# from retrying in lockstep.
MUTEX_ACQUIRE_SLEEP_S = 0.5
MUTEX_ACQUIRE_JITTER_S = 0.1


class NoTaskAvailable(Exception):
    """Raised by task_get when no queue it polled has work ready."""


class TaskStateError(RuntimeError):
    """Raised when an operation does not match a task's current state."""


class MutexNotHeld(RuntimeError):
    """Raised when an operation needs a mutex that is free or held by another."""


@contextmanager
def translate_grpc_error(
    not_found: type[Exception] = KeyError,
    failed_precondition: type[Exception] = TaskStateError,
) -> Iterator[None]:
    """Re-raise gRPC status codes as the exceptions this client documents.

    `not_found` overrides what NOT_FOUND maps to,
    because the code means "no such key" on most RPCs
    but "no task is ready" on TaskGet.
    `failed_precondition` does the same for the code
    the server uses to refuse an operation on state or ownership grounds.
    That code covers four cases:

    * A task that is not Running, or is held by another worker, on TaskDone.
    * A task that is not Running on TaskGetWorkerId.
    * A mutex that is free, or held by another worker, on MutexRelease.
    * A mutex that is free on MutexGetWorkerId.
    """
    try:
        yield
    except grpc.RpcError as e:
        if e.code() == grpc.StatusCode.NOT_FOUND:
            raise not_found(e.details())
        elif e.code() == grpc.StatusCode.ALREADY_EXISTS:
            raise ValueError(e.details())
        elif e.code() == grpc.StatusCode.INVALID_ARGUMENT:
            raise ValueError(e.details())
        elif e.code() == grpc.StatusCode.UNAVAILABLE:
            raise TimeoutError(e.details())
        elif e.code() == grpc.StatusCode.DEADLINE_EXCEEDED:
            raise TimeoutError(e.details())
        elif e.code() == grpc.StatusCode.FAILED_PRECONDITION:
            raise failed_precondition(e.details())
        elif e.code() == grpc.StatusCode.RESOURCE_EXHAUSTED:
            # In practice this is a message larger than MAX_MESSAGE_SIZE_BYTES,
            # that is, a caller-side size problem,
            # so it reads as a ValueError.
            raise ValueError(e.details())
        else:
            raise


def as_queue_list(queue: str | list[str]) -> list[str]:
    """Return the queue names of a queue argument, whether one or several."""
    # Every RPC that takes queues accepts one name or several,
    # and the proto field is repeated either way.
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


def time_series_get_request(
    key: str,
    start_time: str | None,
    end_time: str | None,
    start_step: int | None,
    end_step: int | None,
) -> TimeSeriesGetRequest:
    """Build a TimeSeriesGetRequest carrying only the bounds that were given.

    This function omits a bound passed as None.
    The caller uses None to say "no restriction on this end".
    """
    request = TimeSeriesGetRequest(key=key)
    if start_time is not None:
        request.start_time = start_time
    if end_time is not None:
        request.end_time = end_time
    if start_step is not None:
        request.start_step = start_step
    if end_step is not None:
        request.end_step = end_step
    return request


def mutex_retry_delay(key: str, deadline: float | None) -> float:
    """Return how long to wait before the next mutex_try_acquire attempt.

    deadline is a time.monotonic() reading, or None for "retry forever".
    Raises TimeoutError once the deadline passes,
    and otherwise shortens the delay
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


class DsServiceClient:
    """A connection to a ds-service server, and the RPCs it offers.

    address defaults to the DS_SERVER_ADDRESS environment variable,
    and the constructor raises KeyError when neither is set.
    The client applies timeout, in seconds, as the deadline of every RPC.
    """

    def __init__(
        self,
        address: str | None = None,
        timeout: float = DEFAULT_RPC_TIMEOUT_S,
    ) -> None:
        if address is None:
            self.address = os.environ["DS_SERVER_ADDRESS"]
        else:
            self.address = address
        self.timeout = timeout

        self.channel = grpc.insecure_channel(self.address, options=GRPC_CLIENT_OPTIONS)
        self.stub = DsServiceStub(self.channel)

    def close(self) -> None:
        """Close the underlying gRPC channel.

        The channel cancels in-flight RPCs.
        """
        self.channel.close()

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
        with translate_grpc_error():
            self.stub.MapSet(MapSetRequest(key=key, value=value), timeout=self.timeout)

    def map_get(self, key: str) -> bytes:
        """Return the value stored under key.

        Raises KeyError if the key does not exist.
        """
        with translate_grpc_error():
            response: MapGetResponse = self.stub.MapGet(
                MapGetRequest(key=key), timeout=self.timeout
            )
            return response.value

    def map_search_key(self, pattern: str) -> list[str]:
        """Return the map keys matching the RE2 regular expression pattern.

        The match is unanchored, so it succeeds on any substring of a key
        unless the pattern anchors itself with ^ and $.
        The server returns the keys in unspecified order.
        Raises ValueError if the pattern does not compile.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.MapSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def task_add(
        self,
        task_id: str,
        parent_task_ids: str | list[str],
        queue: str | list[str],
        priority: float,
        function: bytes,
        input: bytes,
    ) -> None:
        """Register a task and enqueue it on each of its queues.

        parent_task_ids names the tasks this one depends on,
        as one id or a list of them.
        An empty list means the task has no parents.
        A task with a parent that is not Finished starts TaskState.Waiting,
        and no queue dispatches it until every parent finishes.
        Every parent must already exist,
        so a graph of tasks is added parents first.
        Raises KeyError for a parent the server does not know,
        and adds nothing in that case.
        A task added with a Canceled parent is added Canceled,
        and one added with a Failed parent is added Failed.
        A task with both is added Failed.

        queue is one queue name or a list of them,
        and the set is fixed for the life of the task.
        function and input are opaque payloads the server only stores.
        Raises ValueError if task_id is already known.
        """
        with translate_grpc_error():
            self.stub.TaskAdd(
                TaskAddRequest(
                    task_id=task_id,
                    parent_task_ids=as_parent_task_id_list(parent_task_ids),
                    queue=as_queue_list(queue),
                    priority=priority,
                    function=function,
                    input=input,
                ),
                timeout=self.timeout,
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

        with translate_grpc_error():
            response: TaskGetStatusResponse = self.stub.TaskGetStatus(
                TaskGetStatusRequest(task_id=task_ids), timeout=self.timeout
            )
            states = list(response.state)
            return states[0] if single else states

    def task_get_output(self, task_id: str) -> bytes:
        """Return the output recorded for a task.

        A task that has not ended has empty output.
        A canceled task reports b"Task canceled",
        and a task failed by one it depends on reports
        b"Dependency failed (task_id=...)", naming the task whose run failed.
        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            response: TaskGetOutputResponse = self.stub.TaskGetOutput(
                TaskGetOutputRequest(task_id=task_id), timeout=self.timeout
            )
            return response.output

    def task_get_count_by_state(self) -> TaskGetCountByStateResponse:
        """Return how many tasks are in each state.

        The response carries waiting, ready, running, finished,
        failed and canceled counts,
        which sum to every task the server knows about.
        """
        with translate_grpc_error():
            return self.stub.TaskGetCountByState(Empty(), timeout=self.timeout)

    def task_get_priority(self, task_id: str) -> float:
        """Return the current priority of an existing task.

        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            response: TaskGetPriorityResponse = self.stub.TaskGetPriority(
                TaskGetPriorityRequest(task_id=task_id), timeout=self.timeout
            )
            return response.priority

    def task_set_priority(self, task_id: str, priority: float) -> None:
        """Change the priority of an existing task.

        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            self.stub.TaskSetPriority(
                TaskSetPriorityRequest(task_id=task_id, priority=priority),
                timeout=self.timeout,
            )

    def task_cancel(self, task_id: str) -> bool:
        """Move a Waiting, Ready or Running task to Canceled.

        Returns True if this call moved the task,
        and False if it was already Finished, Failed or Canceled
        and so was left alone.
        Canceling a task cancels every task waiting on it,
        and every task waiting on those.
        Each task canceled reports b"Task canceled" as its output.
        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            response: TaskCancelResponse = self.stub.TaskCancel(
                TaskCancelRequest(task_id=task_id), timeout=self.timeout
            )
            return response.success

    def task_get_worker_id(self, task_id: str) -> str:
        """Return the worker holding a Running task.

        Raises KeyError for a task_id the server does not know,
        and TaskStateError if the task is not Running.
        """
        with translate_grpc_error():
            response: TaskGetWorkerIdResponse = self.stub.TaskGetWorkerId(
                TaskGetWorkerIdRequest(task_id=task_id), timeout=self.timeout
            )
            return response.worker_id

    def task_search_id(self, pattern: str) -> list[str]:
        """Return the task ids matching the RE2 pattern.

        Same semantics as map_search_key, over the task ids.
        The server searches tasks in every state.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.TaskSearchId(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def task_get(self, worker_id: str, queue: str | list[str]) -> TaskGetResponse:
        """Claim a task for worker_id from the first queue holding one.

        The server tries the queues in the order given.
        Raises NoTaskAvailable, not TimeoutError,
        when none of them has a task ready.
        """
        # A distinct exception keeps idle work
        # distinguishable from an unreachable server.
        with translate_grpc_error(not_found=NoTaskAvailable):
            return self.stub.TaskGet(
                TaskGetRequest(worker_id=worker_id, queue=as_queue_list(queue)),
                timeout=self.timeout,
            )

    def task_done(
        self, task_id: str, worker_id: str, output: bytes, failed: bool = False
    ) -> None:
        """Record a task's output and mark it Finished, or Failed.

        worker_id must be the one that claimed the task through task_get.
        Raises KeyError for a task_id the server does not know,
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
        with translate_grpc_error():
            self.stub.TaskDone(
                TaskDoneRequest(
                    task_id=task_id,
                    output=output,
                    worker_id=worker_id,
                    failed=failed,
                ),
                timeout=self.timeout,
            )

    def journal_size(self, key: str) -> int:
        """Return the number of entries in a journal.

        A journal that does not exist has size 0.
        """
        with translate_grpc_error():
            response: JournalSizeResponse = self.stub.JournalSize(
                JournalSizeRequest(key=key), timeout=self.timeout
            )
            return response.size

    def journal_read(self, key: str, start: int, end: int) -> list[bytes]:
        """Return the entries in the half-open index range [start, end).

        The range is clamped to the journal's bounds,
        so a read past the end returns only the entries that exist.
        An empty range returns an empty list rather than raising.
        A journal that does not exist also returns an empty list.
        """
        with translate_grpc_error():
            response: JournalReadResponse = self.stub.JournalRead(
                JournalReadRequest(key=key, start=start, end=end), timeout=self.timeout
            )
            return list(response.entry)

    def journal_append(self, key: str, value: bytes) -> None:
        """Append one entry to a journal.

        The server creates the journal if it does not exist.
        """
        with translate_grpc_error():
            self.stub.JournalAppend(
                JournalAppendRequest(key=key, value=value), timeout=self.timeout
            )

    def journal_search_key(self, pattern: str) -> list[str]:
        """Return the journal keys matching the RE2 pattern.

        Same semantics as map_search_key, over the journal key space.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.JournalSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def time_series_append(
        self, key: str, value: float, datetime: str, step: int = 0
    ) -> None:
        """Append a point to a series.

        The server creates the series if it does not exist.
        datetime is an ISO 8601 UTC string.
        The server accepts the Z form, an offset form, and a bare datetime.
        Raises ValueError if it does not parse.
        """
        with translate_grpc_error():
            self.stub.TimeSeriesAppend(
                TimeSeriesAppendRequest(
                    key=key, value=value, datetime=datetime, step=step
                ),
                timeout=self.timeout,
            )

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
        The server returns the points in the order the caller appended them.
        A key that does not exist returns an empty list.
        Raises ValueError if start_time or end_time does not parse.
        """
        request = time_series_get_request(
            key, start_time, end_time, start_step, end_step
        )

        with translate_grpc_error():
            response: TimeSeriesGetResponse = self.stub.TimeSeriesGet(
                request, timeout=self.timeout
            )
            return list(response.point)

    def time_series_search_key(self, pattern: str) -> list[str]:
        """Return the series keys matching the RE2 pattern.

        Same semantics as map_search_key, over the keys of the time series.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.TimeSeriesSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def mutex_try_acquire(self, key: str, worker_id: str) -> bool:
        """Try once to acquire a mutex on behalf of worker_id.

        Returns True if this call acquired it,
        which records worker_id as its holder.
        Returns False if the mutex is already held,
        which includes worker_id holding it already:
        the lock is not reentrant.
        """
        with translate_grpc_error():
            response: MutexTryAcquireResponse = self.stub.MutexTryAcquire(
                MutexTryAcquireRequest(key=key, worker_id=worker_id),
                timeout=self.timeout,
            )
            return response.acquired

    def mutex_release(self, key: str, worker_id: str) -> None:
        """Release a mutex held by worker_id.

        Raises MutexNotHeld if worker_id is not its holder,
        which includes a mutex that is already free
        and one that does not exist.
        """
        with translate_grpc_error(failed_precondition=MutexNotHeld):
            self.stub.MutexRelease(
                MutexReleaseRequest(key=key, worker_id=worker_id), timeout=self.timeout
            )

    def mutex_get_worker_id(self, key: str) -> str:
        """Return the worker holding the mutex.

        Raises KeyError if the mutex does not exist,
        and MutexNotHeld if it exists but is free.
        The mutex does not exist until a mutex_try_acquire call names the key.
        """
        with translate_grpc_error(failed_precondition=MutexNotHeld):
            response: MutexGetWorkerIdResponse = self.stub.MutexGetWorkerId(
                MutexGetWorkerIdRequest(key=key), timeout=self.timeout
            )
            return response.worker_id

    def mutex_search_key(self, pattern: str) -> list[str]:
        """Return the mutex keys matching the RE2 pattern.

        Same semantics as map_search_key, over the mutex key space.
        A key exists from the first mutex_try_acquire that names it,
        whether or not that call acquired it.
        A free mutex is listed like a held one.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.MutexSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def mutex_acquire(
        self, key: str, worker_id: str, timeout: float | None = None
    ) -> None:
        """Block until worker_id acquires the mutex.

        Raises TimeoutError once timeout seconds elapse.
        With timeout None, it retries forever.
        This timeout bounds the whole loop, sleeps included.
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
        with translate_grpc_error():
            response: CounterGetNextValueResponse = self.stub.CounterGetNextValue(
                CounterGetNextValueRequest(key=key), timeout=self.timeout
            )
            return response.value

    def counter_get_current_value(self, key: str) -> int:
        """Return a counter's current value without advancing it.

        A counter that does not exist reads as 0 and is not created.
        """
        with translate_grpc_error():
            response: CounterGetCurrentValueResponse = self.stub.CounterGetCurrentValue(
                CounterGetCurrentValueRequest(key=key), timeout=self.timeout
            )
            return response.value

    def counter_search_key(self, pattern: str) -> list[str]:
        """Return the counter keys matching the RE2 pattern.

        Same semantics as map_search_key, over the counter key space.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.CounterSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)


class DsServiceClientAsync:
    """An asyncio connection to a ds-service server, and the RPCs it offers.

    This class offers the same API as DsServiceClient.
    Each method is a coroutine, and `async with` replaces `with`.
    The exceptions each method documents, and the meaning of every argument,
    are unchanged.
    The constructor requires a running event loop.
    """

    def __init__(
        self,
        address: str | None = None,
        timeout: float = DEFAULT_RPC_TIMEOUT_S,
    ) -> None:
        if address is None:
            self.address = os.environ["DS_SERVER_ADDRESS"]
        else:
            self.address = address
        self.timeout = timeout

        # grpc.aio binds the channel to the loop that is current,
        # and this constructor creates the channel rather than the first RPC.
        self.channel = grpc.aio.insecure_channel(
            self.address, options=GRPC_CLIENT_OPTIONS
        )
        self.stub = DsServiceStub(self.channel)

    async def close(self) -> None:
        """Close the underlying gRPC channel.

        The channel cancels in-flight RPCs.
        """
        await self.channel.close()

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
        with translate_grpc_error():
            await self.stub.MapSet(
                MapSetRequest(key=key, value=value), timeout=self.timeout
            )

    async def map_get(self, key: str) -> bytes:
        """Return the value stored under key.

        Raises KeyError if the key does not exist.
        """
        with translate_grpc_error():
            response: MapGetResponse = await self.stub.MapGet(
                MapGetRequest(key=key), timeout=self.timeout
            )
            return response.value

    async def map_search_key(self, pattern: str) -> list[str]:
        """Return the map keys matching the RE2 regular expression pattern.

        The match is unanchored, so it succeeds on any substring of a key
        unless the pattern anchors itself with ^ and $.
        The server returns the keys in unspecified order.
        Raises ValueError if the pattern does not compile.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = await self.stub.MapSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    async def task_add(
        self,
        task_id: str,
        parent_task_ids: str | list[str],
        queue: str | list[str],
        priority: float,
        function: bytes,
        input: bytes,
    ) -> None:
        """Register a task and enqueue it on each of its queues.

        parent_task_ids names the tasks this one depends on,
        as one id or a list of them.
        An empty list means the task has no parents.
        A task with a parent that is not Finished starts TaskState.Waiting,
        and no queue dispatches it until every parent finishes.
        Every parent must already exist,
        so a graph of tasks is added parents first.
        Raises KeyError for a parent the server does not know,
        and adds nothing in that case.
        A task added with a Canceled parent is added Canceled,
        and one added with a Failed parent is added Failed.
        A task with both is added Failed.

        queue is one queue name or a list of them,
        and the set is fixed for the life of the task.
        function and input are opaque payloads the server only stores.
        Raises ValueError if task_id is already known.
        """
        with translate_grpc_error():
            await self.stub.TaskAdd(
                TaskAddRequest(
                    task_id=task_id,
                    parent_task_ids=as_parent_task_id_list(parent_task_ids),
                    queue=as_queue_list(queue),
                    priority=priority,
                    function=function,
                    input=input,
                ),
                timeout=self.timeout,
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

        with translate_grpc_error():
            response: TaskGetStatusResponse = await self.stub.TaskGetStatus(
                TaskGetStatusRequest(task_id=task_ids), timeout=self.timeout
            )
            states = list(response.state)
            return states[0] if single else states

    async def task_get_output(self, task_id: str) -> bytes:
        """Return the output recorded for a task.

        A task that has not ended has empty output.
        A canceled task reports b"Task canceled",
        and a task failed by one it depends on reports
        b"Dependency failed (task_id=...)", naming the task whose run failed.
        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            response: TaskGetOutputResponse = await self.stub.TaskGetOutput(
                TaskGetOutputRequest(task_id=task_id), timeout=self.timeout
            )
            return response.output

    async def task_get_count_by_state(self) -> TaskGetCountByStateResponse:
        """Return how many tasks are in each state.

        The response carries waiting, ready, running, finished,
        failed and canceled counts,
        which sum to every task the server knows about.
        """
        with translate_grpc_error():
            return await self.stub.TaskGetCountByState(Empty(), timeout=self.timeout)

    async def task_get_priority(self, task_id: str) -> float:
        """Return the current priority of an existing task.

        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            response: TaskGetPriorityResponse = await self.stub.TaskGetPriority(
                TaskGetPriorityRequest(task_id=task_id), timeout=self.timeout
            )
            return response.priority

    async def task_set_priority(self, task_id: str, priority: float) -> None:
        """Change the priority of an existing task.

        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            await self.stub.TaskSetPriority(
                TaskSetPriorityRequest(task_id=task_id, priority=priority),
                timeout=self.timeout,
            )

    async def task_cancel(self, task_id: str) -> bool:
        """Move a Waiting, Ready or Running task to Canceled.

        Returns True if this call moved the task,
        and False if it was already Finished, Failed or Canceled
        and so was left alone.
        Canceling a task cancels every task waiting on it,
        and every task waiting on those.
        Each task canceled reports b"Task canceled" as its output.
        Raises KeyError for a task_id the server does not know.
        """
        with translate_grpc_error():
            response: TaskCancelResponse = await self.stub.TaskCancel(
                TaskCancelRequest(task_id=task_id), timeout=self.timeout
            )
            return response.success

    async def task_get_worker_id(self, task_id: str) -> str:
        """Return the worker holding a Running task.

        Raises KeyError for a task_id the server does not know,
        and TaskStateError if the task is not Running.
        """
        with translate_grpc_error():
            response: TaskGetWorkerIdResponse = await self.stub.TaskGetWorkerId(
                TaskGetWorkerIdRequest(task_id=task_id), timeout=self.timeout
            )
            return response.worker_id

    async def task_search_id(self, pattern: str) -> list[str]:
        """Return the task ids matching the RE2 pattern.

        Same semantics as map_search_key, over the task ids.
        The server searches tasks in every state.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = await self.stub.TaskSearchId(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    async def task_get(self, worker_id: str, queue: str | list[str]) -> TaskGetResponse:
        """Claim a task for worker_id from the first queue holding one.

        The server tries the queues in the order given.
        Raises NoTaskAvailable, not TimeoutError,
        when none of them has a task ready.
        """
        # A distinct exception keeps idle work
        # distinguishable from an unreachable server.
        with translate_grpc_error(not_found=NoTaskAvailable):
            return await self.stub.TaskGet(
                TaskGetRequest(worker_id=worker_id, queue=as_queue_list(queue)),
                timeout=self.timeout,
            )

    async def task_done(
        self, task_id: str, worker_id: str, output: bytes, failed: bool = False
    ) -> None:
        """Record a task's output and mark it Finished, or Failed.

        worker_id must be the one that claimed the task through task_get.
        Raises KeyError for a task_id the server does not know,
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
        with translate_grpc_error():
            await self.stub.TaskDone(
                TaskDoneRequest(
                    task_id=task_id,
                    output=output,
                    worker_id=worker_id,
                    failed=failed,
                ),
                timeout=self.timeout,
            )

    async def journal_size(self, key: str) -> int:
        """Return the number of entries in a journal.

        A journal that does not exist has size 0.
        """
        with translate_grpc_error():
            response: JournalSizeResponse = await self.stub.JournalSize(
                JournalSizeRequest(key=key), timeout=self.timeout
            )
            return response.size

    async def journal_read(self, key: str, start: int, end: int) -> list[bytes]:
        """Return the entries in the half-open index range [start, end).

        The range is clamped to the journal's bounds,
        so a read past the end returns only the entries that exist.
        An empty range returns an empty list rather than raising.
        A journal that does not exist also returns an empty list.
        """
        with translate_grpc_error():
            response: JournalReadResponse = await self.stub.JournalRead(
                JournalReadRequest(key=key, start=start, end=end), timeout=self.timeout
            )
            return list(response.entry)

    async def journal_append(self, key: str, value: bytes) -> None:
        """Append one entry to a journal.

        The server creates the journal if it does not exist.
        """
        with translate_grpc_error():
            await self.stub.JournalAppend(
                JournalAppendRequest(key=key, value=value), timeout=self.timeout
            )

    async def journal_search_key(self, pattern: str) -> list[str]:
        """Return the journal keys matching the RE2 pattern.

        Same semantics as map_search_key, over the journal key space.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = await self.stub.JournalSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    async def time_series_append(
        self, key: str, value: float, datetime: str, step: int = 0
    ) -> None:
        """Append a point to a series.

        The server creates the series if it does not exist.
        datetime is an ISO 8601 UTC string.
        The server accepts the Z form, an offset form, and a bare datetime.
        Raises ValueError if it does not parse.
        """
        with translate_grpc_error():
            await self.stub.TimeSeriesAppend(
                TimeSeriesAppendRequest(
                    key=key, value=value, datetime=datetime, step=step
                ),
                timeout=self.timeout,
            )

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
        The server returns the points in the order the caller appended them.
        A key that does not exist returns an empty list.
        Raises ValueError if start_time or end_time does not parse.
        """
        request = time_series_get_request(
            key, start_time, end_time, start_step, end_step
        )

        with translate_grpc_error():
            response: TimeSeriesGetResponse = await self.stub.TimeSeriesGet(
                request, timeout=self.timeout
            )
            return list(response.point)

    async def time_series_search_key(self, pattern: str) -> list[str]:
        """Return the series keys matching the RE2 pattern.

        Same semantics as map_search_key, over the keys of the time series.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = await self.stub.TimeSeriesSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    async def mutex_try_acquire(self, key: str, worker_id: str) -> bool:
        """Try once to acquire a mutex on behalf of worker_id.

        Returns True if this call acquired it,
        which records worker_id as its holder.
        Returns False if the mutex is already held,
        which includes worker_id holding it already:
        the lock is not reentrant.
        """
        with translate_grpc_error():
            response: MutexTryAcquireResponse = await self.stub.MutexTryAcquire(
                MutexTryAcquireRequest(key=key, worker_id=worker_id),
                timeout=self.timeout,
            )
            return response.acquired

    async def mutex_release(self, key: str, worker_id: str) -> None:
        """Release a mutex held by worker_id.

        Raises MutexNotHeld if worker_id is not its holder,
        which includes a mutex that is already free
        and one that does not exist.
        """
        with translate_grpc_error(failed_precondition=MutexNotHeld):
            await self.stub.MutexRelease(
                MutexReleaseRequest(key=key, worker_id=worker_id), timeout=self.timeout
            )

    async def mutex_get_worker_id(self, key: str) -> str:
        """Return the worker holding the mutex.

        Raises KeyError if the mutex does not exist,
        and MutexNotHeld if it exists but is free.
        The mutex does not exist until a mutex_try_acquire call names the key.
        """
        with translate_grpc_error(failed_precondition=MutexNotHeld):
            response: MutexGetWorkerIdResponse = await self.stub.MutexGetWorkerId(
                MutexGetWorkerIdRequest(key=key), timeout=self.timeout
            )
            return response.worker_id

    async def mutex_search_key(self, pattern: str) -> list[str]:
        """Return the mutex keys matching the RE2 pattern.

        Same semantics as map_search_key, over the mutex key space.
        A key exists from the first mutex_try_acquire that names it,
        whether or not that call acquired it.
        A free mutex is listed like a held one.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = await self.stub.MutexSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    async def mutex_acquire(
        self, key: str, worker_id: str, timeout: float | None = None
    ) -> None:
        """Wait until worker_id acquires the mutex.

        Raises TimeoutError once timeout seconds elapse.
        With timeout None, it retries forever.
        This timeout bounds the whole loop, sleeps included.
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
        with translate_grpc_error():
            response: CounterGetNextValueResponse = await self.stub.CounterGetNextValue(
                CounterGetNextValueRequest(key=key), timeout=self.timeout
            )
            return response.value

    async def counter_get_current_value(self, key: str) -> int:
        """Return a counter's current value without advancing it.

        A counter that does not exist reads as 0 and is not created.
        """
        with translate_grpc_error():
            response: CounterGetCurrentValueResponse = (
                await self.stub.CounterGetCurrentValue(
                    CounterGetCurrentValueRequest(key=key), timeout=self.timeout
                )
            )
            return response.value

    async def counter_search_key(self, pattern: str) -> list[str]:
        """Return the counter keys matching the RE2 pattern.

        Same semantics as map_search_key, over the counter key space.
        """
        with translate_grpc_error():
            response: SearchKeyResponse = await self.stub.CounterSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)
