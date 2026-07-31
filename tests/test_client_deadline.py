"""Tests for the per-RPC deadline the client applies to every call."""

import socket
import time

import grpc
import pytest

from ds_service_client import DsServiceClient
from ds_service_client.client import DEFAULT_RPC_TIMEOUT_S, translate_grpc_error


class _FakeRpcError(grpc.RpcError):
    """Stands in for a real RpcError so the mapping can be checked directly."""

    def __init__(self, code, details):
        self._code = code
        self._details = details

    def code(self):
        return self._code

    def details(self):
        return self._details


@pytest.fixture
def mute_address():
    """A listening socket that accepts connections but never speaks gRPC.

    The kernel completes the TCP handshake from the listen backlog on its own,
    so the client connects and then sits waiting on a response
    that never arrives -- which is what the deadline has to cut off.
    Nothing ever calls accept(), so no server thread is needed.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        sock.listen(8)
        yield f"127.0.0.1:{sock.getsockname()[1]}"


def test_client_deadline_is_overridable(server):
    assert DsServiceClient(server, timeout=1.5).timeout == 1.5


def test_deadline_exceeded_maps_to_timeout_error():
    with pytest.raises(TimeoutError):
        with translate_grpc_error():
            raise _FakeRpcError(grpc.StatusCode.DEADLINE_EXCEEDED, "too slow")


def test_unresponsive_server_raises_timeout_error(mute_address):
    client = DsServiceClient(mute_address, timeout=0.5)
    try:
        start = time.monotonic()
        with pytest.raises(TimeoutError):
            client.map_get("anything")
        elapsed = time.monotonic() - start
    finally:
        client.close()

    # Bounded on both sides on purpose.
    # Without the deadline the call still eventually raises TimeoutError
    # -- the connection fails with UNAVAILABLE,
    # which maps to the same exception
    # -- so only the timing distinguishes
    # "the deadline cut it off" from "it failed for some other reason".
    assert 0.4 <= elapsed < 5.0


def test_deadline_does_not_leak_grpc_errors(mute_address):
    # The caller should never have to know about grpc's exception types.
    client = DsServiceClient(mute_address, timeout=0.5)
    try:
        start = time.monotonic()
        with pytest.raises(TimeoutError) as excinfo:
            client.map_set("k", b"v")
        elapsed = time.monotonic() - start
        assert not isinstance(excinfo.value, grpc.RpcError)
    finally:
        client.close()

    assert 0.4 <= elapsed < 5.0


def test_calls_still_succeed_under_the_default_deadline(client):
    # A normal round-trip is nowhere near the deadline.
    client.map_set("k", b"v")
    assert client.map_get("k") == b"v"
