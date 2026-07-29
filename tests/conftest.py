"""Pytest harness for ds-service.

Each test runs against a freshly started ``ds-service`` process.
"""

import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

import pytest

from ds_service_client import Client

STARTUP_TIMEOUT_S = 15.0
STARTUP_POLL_INTERVAL_S = 0.005
GRPC_PROBE_TIMEOUT_S = 15.0
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


def _wait_until_listening(host: str, port: int, proc: subprocess.Popen) -> bool:
    """Poll the server's port until a TCP connection succeeds.

    A plain TCP probe is much faster than ``grpc.channel_ready_future``:
    gRPC applies exponential connection backoff after a failed attempt,
    so a probe that starts before the server has bound its port
    can sit idle in backoff long after the server is actually up.
    """
    deadline = time.monotonic() + STARTUP_TIMEOUT_S
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection((host, port), timeout=STARTUP_TIMEOUT_S):
                return True
        except OSError:
            time.sleep(STARTUP_POLL_INTERVAL_S)
    return False


@pytest.fixture
def server_binary() -> Path:
    """Path to the ds-service binary under test."""
    return _find_binary()


@pytest.fixture
def server_process():
    """Start a ds-service process on a free port and yield (proc, address).

    Most tests want just the address and use the ``server`` fixture;
    this one is for tests that drive the process itself, such as signalling it.
    """
    binary = _find_binary()
    host = "127.0.0.1"
    port = _free_port()
    address = f"{host}:{port}"

    proc = subprocess.Popen(
        [str(binary), "--address", address],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    if not _wait_until_listening(host, port, proc):
        proc.terminate()
        output = proc.communicate()[0]
        raise RuntimeError(
            f"ds-service did not start listening on {address} "
            f"within {STARTUP_TIMEOUT_S}s.\n{output}"
        )

    # The port accepts connections by now,
    # so this first gRPC attempt connects immediately
    # and never enters the exponential reconnect backoff
    # that made a pre-listen probe so slow.
    # A read-only RPC also confirms the service is registered and answering,
    # which a bound port alone does not.
    #
    # Two deliberate choices here:
    #   - Not grpc.channel_ready_future():
    #     it registers a connectivity-state watcher
    #     that makes the subsequent channel close block ~200ms per test.
    #     An RPC round-trip proves more and costs ~1ms.
    #   - Given a short per-RPC deadline rather than the client default,
    #     so a port that accepts but never speaks gRPC
    #     fails the fixture promptly instead of stalling it for minutes.
    try:
        probe = Client(address, timeout=GRPC_PROBE_TIMEOUT_S)
        try:
            probe.task_get_count_by_state()
        finally:
            probe.close()
    except Exception as exc:
        proc.terminate()
        output = proc.communicate()[0]
        raise RuntimeError(
            f"ds-service is listening on {address} but did not answer a gRPC "
            f"request within {GRPC_PROBE_TIMEOUT_S}s: {exc!r}\n{output}"
        ) from exc

    try:
        yield proc, address
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=SHUTDOWN_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


@pytest.fixture
def server(server_process):
    """The address of a running ds-service process.

    A fresh process per test keeps the (non-persistent) server state isolated.
    """
    _, address = server_process
    return address


@pytest.fixture
def client(server):
    """A connected Client for the per-test server."""
    c = Client(server)
    try:
        yield c
    finally:
        c.close()
