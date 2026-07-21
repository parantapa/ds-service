"""Pytest harness for ds-service.

Each test runs against a freshly started ``ds-service`` process.
"""

import os
import shutil
import socket
import subprocess
from pathlib import Path

import grpc
import pytest

from ds_service_client import Client

STARTUP_TIMEOUT_S = 15.0
SHUTDOWN_TIMEOUT_S = 5.0


def _find_binary() -> Path:
    """Locate the ds-service binary via DS_SERVICE_BIN or the PATH.

    Set DS_SERVICE_BIN to an explicit path,
    or make sure a built `ds-service` is on the PATH.
    """
    override = os.environ.get("DS_SERVICE_BIN")
    if override:
        path = Path(override)
        if not path.is_file():
            raise FileNotFoundError(f"DS_SERVICE_BIN points at a missing file: {path}")
        return path

    found = shutil.which("ds-service")
    if found:
        return Path(found)

    raise FileNotFoundError(
        "Could not find the ds-service binary. "
        "Set DS_SERVICE_BIN to its path, "
        "or put a built ds-service on the PATH."
    )


def _free_port() -> int:
    """Reserve an ephemeral port and return it for the server to bind to."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture
def server():
    """Start a ds-service process on a free port and yield its address.

    The server is torn down at the end of the test.
    A fresh process per test keeps the (non-persistent) server state isolated.
    """
    binary = _find_binary()
    address = f"127.0.0.1:{_free_port()}"

    proc = subprocess.Popen(
        [str(binary), "--address", address],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    channel = grpc.insecure_channel(address)
    try:
        grpc.channel_ready_future(channel).result(timeout=STARTUP_TIMEOUT_S)
    except grpc.FutureTimeoutError:
        proc.terminate()
        output = proc.communicate()[0]
        raise RuntimeError(
            f"ds-service did not become ready within {STARTUP_TIMEOUT_S}s.\n{output}"
        )
    finally:
        channel.close()

    try:
        yield address
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=SHUTDOWN_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


@pytest.fixture
def client(server):
    """A connected Client for the per-test server."""
    c = Client(server)
    try:
        yield c
    finally:
        c.close()
