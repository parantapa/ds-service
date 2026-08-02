"""Tests for DsServiceServer, the helper that runs temporary servers.

The rest of the suite starts its servers through this class,
so a fault here shows up everywhere at once.
"""

import ifaddr
import pytest

from ds_service_client import DsServiceClient, DsServiceServer
from ds_service_client.server import (
    DEFAULT_DS_SERVICE_BIN,
    DS_SERVICE_BIN_ENV_VAR,
    resolve_ds_service_bin,
    resolve_interface_ipv4,
)


def test_blank_env_var_falls_back_to_the_default(monkeypatch):
    """An exported but empty DS_SERVICE_BIN means "unset", not "".

    Taken literally the command would start at `--address`,
    and the failure would name that flag as the missing executable.
    """
    monkeypatch.setenv(DS_SERVICE_BIN_ENV_VAR, "")
    assert resolve_ds_service_bin() == DEFAULT_DS_SERVICE_BIN

    monkeypatch.setenv(DS_SERVICE_BIN_ENV_VAR, "   ")
    assert resolve_ds_service_bin() == DEFAULT_DS_SERVICE_BIN

    monkeypatch.delenv(DS_SERVICE_BIN_ENV_VAR)
    assert resolve_ds_service_bin() == DEFAULT_DS_SERVICE_BIN


def test_explicit_binary_wins_over_the_env_var(monkeypatch):
    monkeypatch.setenv(DS_SERVICE_BIN_ENV_VAR, "from-the-environment")
    assert resolve_ds_service_bin("explicit") == "explicit"
    assert resolve_ds_service_bin("") == "from-the-environment"


def test_interface_resolves_to_its_ipv4_address(loopback_interface):
    assert resolve_interface_ipv4(loopback_interface) == "127.0.0.1"


def test_unknown_interface_is_refused():
    """An interface that is not here has no address to bind."""
    with pytest.raises(ValueError, match="No interface named"):
        resolve_interface_ipv4("definitely-not-an-interface")


def test_interface_without_an_ipv4_address_is_refused(monkeypatch):
    """IPv6-only is as unusable as absent: the address is `host:port`."""
    ipv6_only = ifaddr.Adapter(
        name="v6only",
        nice_name="v6only",
        ips=[ifaddr.IP(ip=("fe80::1", 0, 0), network_prefix=64, nice_name="v6only")],
    )
    monkeypatch.setattr(ifaddr, "get_adapters", lambda: [ipv6_only])

    with pytest.raises(ValueError, match="no IPv4 address"):
        resolve_interface_ipv4("v6only")


def test_server_binds_the_address_of_its_interface(loopback_interface):
    with DsServiceServer(loopback_interface) as ds_server:
        assert ds_server.interface == loopback_interface
        assert ds_server.host == "127.0.0.1"
        assert ds_server.address == f"127.0.0.1:{ds_server.port}"


def test_constructing_with_an_unknown_interface_starts_nothing():
    """The interface is resolved before any process is started."""
    with pytest.raises(ValueError):
        DsServiceServer("definitely-not-an-interface")


def test_port_zero_means_an_ephemeral_port(loopback_interface):
    """0 is the usual way to ask the kernel for a port, as is None."""
    with DsServiceServer(loopback_interface, port=0) as ds_server:
        assert ds_server.port != 0
        ds_server.wait_until_ready(timeout=15)

        ds_client = DsServiceClient(ds_server.address)
        try:
            ds_client.map_set("k", b"v")
            assert ds_client.map_get("k") == b"v"
        finally:
            ds_client.close()


def test_explicit_port_already_in_use_is_refused(server, loopback_interface):
    """Starting on an occupied port must fail, not silently adopt it.

    The server that loses the race for the port exits,
    while the port keeps accepting connections,
    so a caller handed that address
    would read and write the other server's state
    believing it were their own.
    """
    _, port = server.rsplit(":", 1)

    with pytest.raises(OSError):
        DsServiceServer(loopback_interface, port=int(port))


def test_failed_start_leaves_the_running_server_alone(
    server, client, loopback_interface
):
    """A refused start must not disturb the server already on that port."""
    client.map_set("owner", b"first-server")

    _, port = server.rsplit(":", 1)
    with pytest.raises(OSError):
        DsServiceServer(loopback_interface, port=int(port))

    assert client.map_get("owner") == b"first-server"


def test_ephemeral_port_server_starts_and_answers(loopback_interface):
    with DsServiceServer(loopback_interface) as ds_server:
        ds_server.wait_until_ready(timeout=15)

        ds_client = DsServiceClient(ds_server.address)
        try:
            ds_client.map_set("k", b"v")
            assert ds_client.map_get("k") == b"v"
        finally:
            ds_client.close()

    assert ds_server.process.poll() is not None


def test_dead_process_reported_as_runtime_error(loopback_interface):
    ds_server = DsServiceServer(loopback_interface, ds_service_bin="/bin/false")
    try:
        with pytest.raises(RuntimeError):
            ds_server.wait_until_ready(timeout=15)
    finally:
        ds_server.close()
