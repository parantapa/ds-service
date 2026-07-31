"""Tests for DsServiceServer, the helper that runs temporary servers.

The rest of the suite starts its servers through this class,
so a fault here shows up everywhere at once.
"""

import pytest

from ds_service_client import DsServiceClient, DsServiceServer


def test_port_zero_means_an_ephemeral_port():
    """0 is the usual way to ask the kernel for a port, as is None."""
    with DsServiceServer(host="127.0.0.1", port=0) as ds_server:
        assert ds_server.port != 0
        ds_server.wait_until_ready(timeout=15)

        ds_client = DsServiceClient(ds_server.address)
        try:
            ds_client.map_set("k", b"v")
            assert ds_client.map_get("k") == b"v"
        finally:
            ds_client.close()


def test_explicit_port_already_in_use_is_refused(server):
    """Starting on an occupied port must fail, not silently adopt it.

    The server that loses the race for the port exits,
    while the port keeps accepting connections,
    so a caller handed that address would read and write
    the other server's state believing it were their own.
    """
    host, port = server.rsplit(":", 1)

    with pytest.raises(OSError):
        DsServiceServer(host=host, port=int(port))


def test_failed_start_leaves_the_running_server_alone(server, client):
    """A refused start must not disturb the server already on that port."""
    client.map_set("owner", b"first-server")

    host, port = server.rsplit(":", 1)
    with pytest.raises(OSError):
        DsServiceServer(host=host, port=int(port))

    assert client.map_get("owner") == b"first-server"


def test_ephemeral_port_server_starts_and_answers():
    with DsServiceServer(host="127.0.0.1") as ds_server:
        ds_server.wait_until_ready(timeout=15)

        ds_client = DsServiceClient(ds_server.address)
        try:
            ds_client.map_set("k", b"v")
            assert ds_client.map_get("k") == b"v"
        finally:
            ds_client.close()

    assert ds_server.process.poll() is not None


def test_dead_process_reported_as_runtime_error():
    ds_server = DsServiceServer(host="127.0.0.1", ds_service_bin="/bin/false")
    try:
        with pytest.raises(RuntimeError):
            ds_server.wait_until_ready(timeout=15)
    finally:
        ds_server.close()
