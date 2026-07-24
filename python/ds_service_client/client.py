"""Ds Service Client."""

import os
import random
import time
from contextlib import contextmanager

from .ds_service_pb2 import *
from .ds_service_pb2_grpc import *

GRPC_CLIENT_OPTIONS = [
    ("grpc.keepalive_time_ms", 120 * 1000),
    ("grpc.keepalive_timeout_ms", 30 * 000),
    ("grpc.http2.max_pings_without_data", 5),
    ("grpc.keepalive_permit_without_calls", 1),
]

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
        else:
            raise


class Client:
    def __init__(self, address: str | None = None):
        if address is None:
            self.address = os.environ["DS_SERVER_ADDRESS"]
        else:
            self.address = address

        self.channel = grpc.insecure_channel(self.address, options=GRPC_CLIENT_OPTIONS)
        self.stub = DsServiceStub(self.channel)

    def close(self):
        self.channel.close()

    def map_set(self, key: str, value: bytes) -> None:
        with translate_grpc_error():
            self.stub.MapSet(MapSetRequest(key=key, value=value))

    def map_get(self, key: str) -> bytes:
        with translate_grpc_error():
            response: MapGetResponse = self.stub.MapGet(MapGetRequest(key=key))
            return response.value

    def map_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.MapSearchKey(
                SearchKeyRequest(pattern=pattern)
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
                )
            )

    def task_status(self, task_id: str) -> TaskStatusResponse:
        with translate_grpc_error():
            return self.stub.TaskStatus(TaskStatusRequest(task_id=task_id))

    def task_get(self, worker_id: str, queue: str | list[str]) -> TaskGetResponse:
        if isinstance(queue, str):
            queue = [queue]

        with translate_grpc_error():
            return self.stub.TaskGet(TaskGetRequest(worker_id=worker_id, queue=queue))

    def task_done(self, task_id: str, output: bytes):
        with translate_grpc_error():
            return self.stub.TaskDone(TaskDoneRequest(task_id=task_id, output=output))

    def requeue(self, timeout_s: float):
        with translate_grpc_error():
            return self.stub.Requeue(RequeueRequest(timeout_s=timeout_s))

    def journal_size(self, key: str) -> int:
        with translate_grpc_error():
            response: JournalSizeResponse = self.stub.JournalSize(
                JournalSizeRequest(key=key)
            )
            return response.size

    def journal_read(self, key: str, start: int, end: int) -> list[bytes]:
        with translate_grpc_error():
            response: JournalReadResponse = self.stub.JournalRead(
                JournalReadRequest(key=key, start=start, end=end)
            )
            return list(response.entry)

    def journal_append(self, key: str, value: bytes) -> None:
        with translate_grpc_error():
            self.stub.JournalAppend(JournalAppendRequest(key=key, value=value))

    def journal_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.JournalSearchKey(
                SearchKeyRequest(pattern=pattern)
            )
            return list(response.key)

    def time_series_append(
        self, key: str, value: float, datetime: str, step: int = 0
    ) -> None:
        with translate_grpc_error():
            self.stub.TimeSeriesAppend(
                TimeSeriesAppendRequest(
                    key=key, value=value, datetime=datetime, step=step
                )
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
            response: TimeSeriesGetResponse = self.stub.TimeSeriesGet(request)
            return list(response.point)

    def time_series_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.TimeSeriesSearchKey(
                SearchKeyRequest(pattern=pattern)
            )
            return list(response.key)

    def mutex_try_acquire(self, key: str) -> bool:
        with translate_grpc_error():
            response: MutexTryAcquireResponse = self.stub.MutexTryAcquire(
                MutexTryAcquireRequest(key=key)
            )
            return response.acquired

    def mutex_release(self, key: str) -> None:
        with translate_grpc_error():
            self.stub.MutexRelease(MutexReleaseRequest(key=key))

    def mutex_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.MutexSearchKey(
                SearchKeyRequest(pattern=pattern)
            )
            return list(response.key)

    def mutex_acquire(self, key: str, timeout: float | None = None) -> None:
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
                CounterGetNextValueRequest(key=key)
            )
            return response.value

    def counter_search_key(self, pattern: str) -> list[str]:
        with translate_grpc_error():
            response: SearchKeyResponse = self.stub.CounterSearchKey(
                SearchKeyRequest(pattern=pattern)
            )
            return list(response.key)
