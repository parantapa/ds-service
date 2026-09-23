"""Tests for translate_error, the client's error-code mapping."""

import pytest

from ds_service_client import (
    MutexNotHeld,
    NoTaskAvailable,
    TaskStateError,
    TransportError,
)
from ds_service_client._ext import ClientError, ErrorCode
from ds_service_client.client import translate_error


# These tests need no server.
# A constructed ClientError carries the code,
# so a test can reach every branch,
# whether or not a real server produces it.
def _raise(code: ErrorCode, message: str = "details") -> None:
    raise ClientError(code, message)


@pytest.mark.parametrize(
    ("code", "expected"),
    [
        (ErrorCode.NotFound, KeyError),
        (ErrorCode.AlreadyExists, ValueError),
        (ErrorCode.InvalidArgument, ValueError),
        (ErrorCode.MessageTooLarge, ValueError),
        (ErrorCode.FailedPrecondition, TaskStateError),
        (ErrorCode.Unavailable, TimeoutError),
        (ErrorCode.DeadlineExceeded, TimeoutError),
        (ErrorCode.Closed, RuntimeError),
        (ErrorCode.Cancelled, TransportError),
        (ErrorCode.Transport, TransportError),
    ],
)
def test_each_code_maps_to_its_exception(code: ErrorCode, expected: type):
    with pytest.raises(expected) as excinfo:
        with translate_error():
            _raise(code, "the message")
    # The message survives, and the original error is chained.
    assert "the message" in str(excinfo.value)
    assert isinstance(excinfo.value.__cause__, ClientError)


def test_every_code_is_mapped():
    # A new ErrorCode must get a deliberate mapping, not fall through unnoticed.
    assert len(ErrorCode) == 10


def test_closed_is_a_plain_runtime_error():
    # TaskStateError and MutexNotHeld are RuntimeErrors too,
    # so a closed client must not raise either of them.
    with pytest.raises(RuntimeError) as excinfo:
        with translate_error():
            _raise(ErrorCode.Closed)
    assert type(excinfo.value) is RuntimeError


def test_failed_precondition_is_overridable_for_mutex_release():
    # mutex_release reports a refused release
    # the same way task_done reports a foreign worker,
    # but it is not a task-state problem.
    with pytest.raises(MutexNotHeld):
        with translate_error(failed_precondition=MutexNotHeld):
            _raise(ErrorCode.FailedPrecondition, "not held")


def test_not_found_is_overridable_for_task_get():
    # task_get reports an idle queue as NotFound,
    # which means "no work", not "no such key".
    with pytest.raises(NoTaskAvailable):
        with translate_error(not_found=NoTaskAvailable):
            _raise(ErrorCode.NotFound, "No tasks available.")


def test_other_exceptions_pass_through():
    with pytest.raises(ZeroDivisionError):
        with translate_error():
            raise ZeroDivisionError
