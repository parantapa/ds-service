"""Tests for translate_grpc_error, the client's status-code mapping.

These check the mapping directly, without a server:
a fake RpcError carries the status code,
so every branch can be reached
whether or not a real server produces it easily.
"""

import grpc
import pytest

from ds_service_client import MutexNotHeld, NoTaskAvailable, TaskStateError
from ds_service_client.client import translate_grpc_error


class _FakeRpcError(grpc.RpcError):
    """Stands in for a real RpcError so the mapping can be checked directly."""

    def __init__(self, code, details):
        self._code = code
        self._details = details

    def code(self):
        return self._code

    def details(self):
        return self._details


def test_deadline_exceeded_maps_to_timeout_error():
    with pytest.raises(TimeoutError):
        with translate_grpc_error():
            raise _FakeRpcError(grpc.StatusCode.DEADLINE_EXCEEDED, "too slow")


def test_resource_exhausted_maps_to_value_error():
    with pytest.raises(ValueError):
        with translate_grpc_error():
            raise _FakeRpcError(grpc.StatusCode.RESOURCE_EXHAUSTED, "too big")


def test_failed_precondition_maps_to_task_state_error():
    with pytest.raises(TaskStateError):
        with translate_grpc_error():
            raise _FakeRpcError(grpc.StatusCode.FAILED_PRECONDITION, "not Running")


def test_failed_precondition_is_overridable_for_mutex_release():
    # MutexRelease reports a refused release
    # the same way TaskDone reports a foreign worker,
    # but it is not a task-state problem.
    with pytest.raises(MutexNotHeld):
        with translate_grpc_error(failed_precondition=MutexNotHeld):
            raise _FakeRpcError(grpc.StatusCode.FAILED_PRECONDITION, "not held")


def test_not_found_maps_to_key_error_by_default():
    with pytest.raises(KeyError):
        with translate_grpc_error():
            raise _FakeRpcError(grpc.StatusCode.NOT_FOUND, "no such key")


def test_not_found_is_overridable_for_task_get():
    # TaskGet reports an idle queue as NOT_FOUND,
    # which means "no work", not "no such key".
    with pytest.raises(NoTaskAvailable):
        with translate_grpc_error(not_found=NoTaskAvailable):
            raise _FakeRpcError(grpc.StatusCode.NOT_FOUND, "No tasks available.")


def test_unavailable_still_maps_to_timeout_error():
    # Reserved for a server that cannot be reached,
    # now that an idle queue no longer uses it.
    with pytest.raises(TimeoutError):
        with translate_grpc_error():
            raise _FakeRpcError(grpc.StatusCode.UNAVAILABLE, "unreachable")
