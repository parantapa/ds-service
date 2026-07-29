"""Tests for the server's handling of shutdown signals."""

import signal

import pytest

from ds_service_client import Client

# Generous next to the server's own grace period,
# which only has an idle server to drain here.
EXIT_TIMEOUT_S = 10.0


@pytest.mark.parametrize("signum", [signal.SIGTERM, signal.SIGINT])
def test_shutdown_signal_exits_cleanly(server_process, signum):
    """SIGTERM and SIGINT shut the server down instead of killing it.

    A signal handled by its default disposition
    would leave the process reporting -signum rather than 0.
    """
    proc, _ = server_process

    proc.send_signal(signum)

    assert proc.wait(timeout=EXIT_TIMEOUT_S) == 0


def test_shutdown_serves_requests_until_signalled(server_process):
    """The server answers normally right up to the shutdown signal."""
    proc, address = server_process

    client = Client(address)
    try:
        client.map_set("key", b"value")
        assert client.map_get("key") == b"value"
    finally:
        client.close()

    proc.send_signal(signal.SIGTERM)

    assert proc.wait(timeout=EXIT_TIMEOUT_S) == 0
