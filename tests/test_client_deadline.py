"""Tests for the deadline the client applies to every call, and for interrupting a call."""

import _thread
import socket
import threading
import time
from collections.abc import Iterator

import pytest

from ds_service_client import DsServiceClient, NoTaskAvailable
from ds_service_client._ext import ClientError


@pytest.fixture
def mute_address() -> Iterator[str]:
    """The address of a listening socket that never answers a call.

    A client connects, and then waits on a response that never arrives.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        # The kernel completes the TCP handshake from the listen backlog
        # on its own.
        # Nothing ever calls accept(), so the test needs no server thread.
        sock.listen(8)
        yield f"127.0.0.1:{sock.getsockname()[1]}"


def test_unresponsive_server_raises_timeout_error(mute_address):
    client = DsServiceClient(mute_address, timeout=0.5)
    try:
        start = time.monotonic()
        with pytest.raises(TimeoutError) as excinfo:
            client.map_get("anything")
        elapsed = time.monotonic() - start
        # The caller never sees the extension module's own exception type.
        assert not isinstance(excinfo.value, ClientError)
    finally:
        client.close()

    # Bounded on both sides on purpose.
    # Without the deadline the call still eventually raises TimeoutError,
    # because the connection fails as unavailable,
    # which maps to the same exception.
    # So only the timing distinguishes
    # "the deadline cut it off" from "it failed for some other reason".
    assert 0.4 <= elapsed < 5.0


def test_task_get_on_an_unreachable_server_is_not_no_task_available():
    # A worker sleeps and retries on NoTaskAvailable,
    # so a server it cannot reach must raise something else.
    # Otherwise the loop polls a dead address forever.
    #
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


def test_keyboard_interrupt_stops_a_waiting_call(mute_address):
    # The call releases the GIL while it waits,
    # and polls for a pending signal about every 100 ms.
    # interrupt_main() raises the same KeyboardInterrupt that Ctrl-C does.
    client = DsServiceClient(mute_address, timeout=60.0)
    timer = threading.Timer(0.3, _thread.interrupt_main)
    try:
        start = time.monotonic()
        timer.start()
        with pytest.raises(KeyboardInterrupt):
            client.map_get("anything")
        elapsed = time.monotonic() - start
    finally:
        timer.cancel()
        client.close()

    # Far below the 60 s deadline.
    assert elapsed < 5.0
