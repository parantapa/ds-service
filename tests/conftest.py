"""Pytest harness for ds-service.

Each test runs against a freshly started ``ds-service`` process.
Starting and stopping it is left to ``ds_service_client.DsServiceServer``.
"""

import ifaddr
import pytest

from ds_service_client import DsServiceClient, DsServiceServer
from ds_service_client.server import resolve_ds_service_bin

STARTUP_TIMEOUT_S = 15
GRPC_PROBE_TIMEOUT_S = 15.0

# The address every test server binds, through its interface.
LOOPBACK_IP = "127.0.0.1"


def _loopback_interface() -> str:
    """The name of the interface holding 127.0.0.1.

    Looked up rather than hardcoded to `lo`,
    because DsServiceServer is given an interface name
    and the name of the loopback one is the platform's business.
    """
    for adapter in ifaddr.get_adapters():
        for ip in adapter.ips:
            if ip.is_IPv4 and ip.ip == LOOPBACK_IP:
                return adapter.name

    raise RuntimeError(f"No interface on this machine holds {LOOPBACK_IP}.")


def _probe_grpc(address: str) -> None:
    """Make one read-only RPC against a server that is already listening.

    A bound port only proves something is listening;
    this proves the service is registered and answering.

    Two deliberate choices here:
      - Not grpc.channel_ready_future():
        it registers a connectivity-state watcher
        that makes the subsequent channel close block ~200ms per test.
        An RPC round-trip proves more and costs ~1ms.
      - Given a short per-RPC deadline rather than the client default,
        so a port that accepts but never speaks gRPC
        fails the fixture promptly
        instead of stalling it for minutes.
    """
    probe = DsServiceClient(address, timeout=GRPC_PROBE_TIMEOUT_S)
    try:
        probe.task_get_count_by_state()
    finally:
        probe.close()


@pytest.fixture
def server_binary() -> str:
    """How to start the server under test.

    Resolved by DsServiceServer's own helper,
    so it may be a whole command line rather than a path:
    split it with ``shlex.split`` before running it.
    """
    return resolve_ds_service_bin()


@pytest.fixture(scope="session")
def loopback_interface() -> str:
    """The interface test servers bind, i.e. the one holding 127.0.0.1.

    Session-scoped because the machine's interfaces
    do not change under the suite.
    """
    return _loopback_interface()


@pytest.fixture
def server_process(loopback_interface):
    """Start a ds-service process on a free port and yield (proc, address).

    Most tests want just the address and use the ``server`` fixture;
    this one is for tests that drive the process itself,
    such as signalling it.
    """
    server = DsServiceServer(loopback_interface)
    try:
        server.wait_until_ready(timeout=STARTUP_TIMEOUT_S)

        try:
            _probe_grpc(server.address)
        except Exception as exc:
            raise RuntimeError(
                f"ds-service is listening on {server.address} but did not "
                f"answer a gRPC request within {GRPC_PROBE_TIMEOUT_S}s: {exc!r}"
            ) from exc

        yield server.process, server.address
    finally:
        server.close()


@pytest.fixture
def server(server_process):
    """The address of a running ds-service process.

    A fresh process per test keeps the (non-persistent) server state isolated.
    """
    _, address = server_process
    return address


@pytest.fixture
def client(server):
    """A connected DsServiceClient for the per-test server."""
    c = DsServiceClient(server)
    try:
        yield c
    finally:
        c.close()
