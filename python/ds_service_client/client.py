"""Ds Service Client."""

import os
import random
import time
from contextlib import contextmanager

from .ds_service_pb2 import *
from .ds_service_pb2_grpc import *

# Largest single request or response accepted, in bytes.
# Must match MAX_MESSAGE_SIZE_BYTES in cpp/ds-service.cpp:
# if the two disagree, one side rejects what the other happily sends.
# gRPC's own default is 4 MiB.
MAX_MESSAGE_SIZE_BYTES = 64 * 1024 * 1024

# Several of these options are only correct
# as a matched pair with the server's channel arguments
# in cpp/ds-service.cpp;
# tests/test_grpc_options.py is what keeps the two sides in step.
GRPC_CLIENT_OPTIONS = [
    # This ping interval must stay above the server's
    # GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS
    # (10s in cpp/ds-service.cpp),
    # or the server answers pings with GOAWAY/ENHANCE_YOUR_CALM
    # and drops the connection.
    # Callers see that as a TimeoutError with no mention of pings.
    ("grpc.keepalive_time_ms", 120 * 1000),
    ("grpc.keepalive_timeout_ms", 30 * 1000),
    # 0 means "unlimited".
    # This caps the number of keepalive pings sent while no RPC is in flight
    # -- it is a total, not a rate --
    # so any finite value makes the client stop pinging
    # on a long-idle connection,
    # which is exactly the connection
    # keepalive_permit_without_calls is meant to protect.
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
MUTEX_ACQUIRE_SLEEP_S = 0.5
MUTEX_ACQUIRE_JITTER_S = 0.1


@contextmanager
def translate_grpc_error():
    try:
        yield
    except grpc.RpcError as e:
        if e.code() == grpc.StatusCode.NOT_FOUND:
            raise KeyError(e.details())
        elif e.code() == grpc.StatusCode.ALREADY_EXISTS:
            raise ValueError(e.details())
        elif e.code() == grpc.StatusCode.INVALID_ARGUMENT:
            raise ValueError(e.details())
        elif e.code() == grpc.StatusCode.UNAVAILABLE:
            raise TimeoutError(e.details())
        elif e.code() == grpc.StatusCode.DEADLINE_EXCEEDED:
            raise TimeoutError(e.details())
        elif e.code() == grpc.StatusCode.RESOURCE_EXHAUSTED:
            # In practice this is a message larger than MAX_MESSAGE_SIZE_BYTES,
            # i.e. a caller-side size problem,
            # so it reads as a ValueError.
            raise ValueError(e.details())
        else:
            raise


class DsServiceClient:
    def __init__(
        self,
        address: str | None = None,
        timeout: float = DEFAULT_RPC_TIMEOUT_S,
    ):
        if address is None:
            self.address = os.environ["DS_SERVER_ADDRESS"]
        else:
            self.address = address
        self.timeout = timeout

        self.channel = grpc.insecure_channel(self.address, options=GRPC_CLIENT_OPTIONS)
        self.stub = DsServiceStub(self.channel)

    def close(self):
        self.channel.close()

    def __enter__(self) -> "DsServiceClient":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()

    def map_set(self, key: str, value: bytes) -> None:
        with translate_grpc_error():
            self.stub.MapSet(MapSetRequest(key=key, value=value), timeout=self.timeout)

    def map_get(self, key: str) -> bytes:
        with translate_grpc_error():
            response: MapGetResponse = self.stub.MapGet(
                MapGetRequest(key=key), timeout=self.timeout
            )
            return response.value

    def map_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.MapSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def task_add(
        self,
        task_id: str,
        queue: str | list[str],
        priority: float,
        function: bytes,
        input: bytes,
    ) -> None:
        if isinstance(queue, str):
            queue = [queue]

        with translate_grpc_error():
            self.stub.TaskAdd(
                TaskAddRequest(
                    task_id=task_id,
                    queue=queue,
                    priority=priority,
                    function=function,
                    input=input,
                ),
                timeout=self.timeout,
            )

    def task_get_status(self, task_id: str | list[str]) -> TaskState | list[TaskState]:
        # A single string returns a single state;
        # a list returns a list of states, one per id in the same order.
        single = isinstance(task_id, str)
        task_ids = [task_id] if single else task_id

        with translate_grpc_error():
            response: TaskGetStatusResponse = self.stub.TaskGetStatus(
                TaskGetStatusRequest(task_id=task_ids), timeout=self.timeout
            )
            states = list(response.state)
            return states[0] if single else states

    def task_get_output(self, task_id: str) -> bytes:
        with translate_grpc_error():
            response: TaskGetOutputResponse = self.stub.TaskGetOutput(
                TaskGetOutputRequest(task_id=task_id), timeout=self.timeout
            )
            return response.output

    def task_get_count_by_state(self) -> TaskGetCountByStateResponse:
        with translate_grpc_error():
            return self.stub.TaskGetCountByState(Empty(), timeout=self.timeout)

    def task_get(self, worker_id: str, queue: str | list[str]) -> TaskGetResponse:
        if isinstance(queue, str):
            queue = [queue]

        with translate_grpc_error():
            return self.stub.TaskGet(
                TaskGetRequest(worker_id=worker_id, queue=queue), timeout=self.timeout
            )

    def task_done(self, task_id: str, output: bytes):
        with translate_grpc_error():
            return self.stub.TaskDone(
                TaskDoneRequest(task_id=task_id, output=output), timeout=self.timeout
            )

    def task_requeue(self, timeout_s: float):
        with translate_grpc_error():
            return self.stub.TaskRequeue(
                TaskRequeueRequest(timeout_s=timeout_s), timeout=self.timeout
            )

    def journal_size(self, key: str) -> int:
        with translate_grpc_error():
            response: JournalSizeResponse = self.stub.JournalSize(
                JournalSizeRequest(key=key), timeout=self.timeout
            )
            return response.size

    def journal_read(self, key: str, start: int, end: int) -> list[bytes]:
        with translate_grpc_error():
            response: JournalReadResponse = self.stub.JournalRead(
                JournalReadRequest(key=key, start=start, end=end), timeout=self.timeout
            )
            return list(response.entry)

    def journal_append(self, key: str, value: bytes) -> None:
        with translate_grpc_error():
            self.stub.JournalAppend(
                JournalAppendRequest(key=key, value=value), timeout=self.timeout
            )

    def journal_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.JournalSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def time_series_append(
        self, key: str, value: float, datetime: str, step: int = 0
    ) -> None:
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
        request = TimeSeriesGetRequest(key=key)
        if start_time is not None:
            request.start_time = start_time
        if end_time is not None:
            request.end_time = end_time
        if start_step is not None:
            request.start_step = start_step
        if end_step is not None:
            request.end_step = end_step

        with translate_grpc_error():
            response: TimeSeriesGetResponse = self.stub.TimeSeriesGet(
                request, timeout=self.timeout
            )
            return list(response.point)

    def time_series_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.TimeSeriesSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def mutex_try_acquire(self, key: str) -> bool:
        with translate_grpc_error():
            response: MutexTryAcquireResponse = self.stub.MutexTryAcquire(
                MutexTryAcquireRequest(key=key), timeout=self.timeout
            )
            return response.acquired

    def mutex_release(self, key: str) -> None:
        with translate_grpc_error():
            self.stub.MutexRelease(MutexReleaseRequest(key=key), timeout=self.timeout)

    def mutex_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.MutexSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)

    def mutex_acquire(self, key: str, timeout: float | None = None) -> None:
        # Note: this timeout bounds the whole acquire loop,
        # including the sleeps between retries.
        # It is unrelated to self.timeout,
        # which is the per-RPC deadline on each underlying mutex_try_acquire.
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if self.mutex_try_acquire(key):
                return

            delay = MUTEX_ACQUIRE_SLEEP_S + random.uniform(
                -MUTEX_ACQUIRE_JITTER_S, MUTEX_ACQUIRE_JITTER_S
            )
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"Timed out acquiring mutex {key!r}.")
                delay = min(delay, remaining)

            time.sleep(delay)

    def counter_get_next_value(self, key: str) -> int:
        with translate_grpc_error():
            response: CounterGetNextValueResponse = self.stub.CounterGetNextValue(
                CounterGetNextValueRequest(key=key), timeout=self.timeout
            )
            return response.value

    def counter_get_current_value(self, key: str) -> int:
        with translate_grpc_error():
            response: CounterGetCurrentValueResponse = self.stub.CounterGetCurrentValue(
                CounterGetCurrentValueRequest(key=key), timeout=self.timeout
            )
            return response.value

    def counter_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.CounterSearchKey(
                SearchKeyRequest(pattern=pattern), timeout=self.timeout
            )
            return list(response.key)
