"""Tests for the per-RPC deadline the client applies to every call."""

import socket
import time

import grpc
import pytest

from ds_service_client import DsServiceClient, NoTaskAvailable


@pytest.fixture
def mute_address():
    """A listening socket that accepts connections but never speaks gRPC.

    The kernel completes the TCP handshake from the listen backlog on its own,
    so the client connects
    and then sits waiting on a response that never arrives
    -- which is what the deadline has to cut off.
    Nothing ever calls accept(), so no server thread is needed.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        sock.listen(8)
        yield f"127.0.0.1:{sock.getsockname()[1]}"


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


def test_task_get_on_an_unreachable_server_is_not_no_task_available():
    """The distinction the worker loop depends on.

    A worker sleeps and retries on NoTaskAvailable,
    so a server it cannot reach must raise something else
    -- otherwise the loop polls a dead address for ever.
    """
    # Bind a port and drop it, so nothing is listening on a known address.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        dead_address = "127.0.0.1:%d" % sock.getsockname()[1]

    client = DsServiceClient(dead_address, timeout=2.0)
    try:
        with pytest.raises(TimeoutError) as excinfo:
            client.task_get(worker_id="w1", queue="work")
        assert not isinstance(excinfo.value, NoTaskAvailable)
    finally:
        client.close()
