"""Tests for the server's handling of shutdown signals."""

import signal

import pytest

# Generous next to the server's own grace period,
# which only has an idle server to drain here.
EXIT_TIMEOUT_S = 10.0


@pytest.mark.parametrize("signum", [signal.SIGTERM, signal.SIGINT])
def test_shutdown_signal_exits_cleanly(server_process, signum):
    proc, _ = server_process

    proc.send_signal(signum)

    # A signal handled by its default disposition kills the process,
    # which then reports -signum rather than 0.
    assert proc.wait(timeout=EXIT_TIMEOUT_S) == 0
