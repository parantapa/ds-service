"""Pytest harness for ds-service."""

import subprocess
from collections.abc import Iterator

import ifaddr
import pytest

from ds_service_client import DsServiceClient, DsServiceServer
from ds_service_client.server import resolve_ds_service_bin

STARTUP_TIMEOUT_S = 15
PROBE_TIMEOUT_S = 15.0

# The address every test server binds, through its interface.
LOOPBACK_IP = "127.0.0.1"


def _loopback_interface() -> str:
    """The name of the interface holding 127.0.0.1."""
    # DsServiceServer takes an interface name,
    # and the name of the loopback interface depends on the platform.
    # So this function looks it up rather than hardcode `lo`.
    for adapter in ifaddr.get_adapters():
        for ip in adapter.ips:
            if ip.is_IPv4 and ip.ip == LOOPBACK_IP:
                return adapter.name

    raise RuntimeError(f"No interface on this machine holds {LOOPBACK_IP}.")


def _probe(address: str) -> None:
    """Make one read-only call against a server that already listens."""
    # A bound port only proves something listens there.
    # This call proves the server registered the service and answers.
    #
    # The deadline is short rather than the client default.
    # A port that accepts but never answers
    # fails the fixture promptly
    # instead of stalling it for minutes.
    probe = DsServiceClient(address, timeout=PROBE_TIMEOUT_S)
    try:
        probe.task_get_count_by_state()
    finally:
        probe.close()


@pytest.fixture
def server_binary() -> str:
    """How to start the server under test.

    It can be a whole command line rather than a path,
    which ``shlex.split`` turns into the arguments to run.
    """
    # DsServiceServer resolves its binary through the same function,
    # so this value and the fixtures' servers cannot disagree.
    return resolve_ds_service_bin()


# Session-scoped because the machine's interfaces
# do not change under the suite.
@pytest.fixture(scope="session")
def loopback_interface() -> str:
    """The interface test servers bind, that is, the one holding 127.0.0.1."""
    return _loopback_interface()


@pytest.fixture
def server_process(
    loopback_interface: str,
) -> Iterator[tuple[subprocess.Popen[bytes], str]]:
    """A ds-service process on a free port, as ``(proc, address)``.

    Most tests want only the address and use the ``server`` fixture.
    This one is for tests that drive the process itself,
    such as signaling it.
    """
    server = DsServiceServer(loopback_interface)
    try:
        server.wait_until_ready(timeout=STARTUP_TIMEOUT_S)

        try:
            _probe(server.address)
        except Exception as exc:
            raise RuntimeError(
                f"ds-service is listening on {server.address} but did not "
                f"answer a request within {PROBE_TIMEOUT_S}s: {exc!r}"
            ) from exc

        yield server.process, server.address
    finally:
        server.close()


@pytest.fixture
def server(server_process: tuple[subprocess.Popen[bytes], str]) -> str:
    """The address of a running ds-service process.

    A fresh process per test keeps the (non-persistent) server state isolated.
    """
    _, address = server_process
    return address


@pytest.fixture
def client(server: str) -> Iterator[DsServiceClient]:
    """A connected DsServiceClient for the per-test server."""
    c = DsServiceClient(server)
    try:
        yield c
    finally:
        c.close()
