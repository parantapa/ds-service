"""Tests for translate_grpc_error, the client's status-code mapping.

These check the mapping directly, without a server:
a fake RpcError carries the status code,
so every branch can be reached
whether or not a real server produces it easily.
"""

import grpc
import pytest

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
