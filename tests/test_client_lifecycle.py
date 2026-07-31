"""Tests for connecting and disconnecting a DsServiceClient."""

import pytest

from ds_service_client import DsServiceClient


def test_client_works_as_a_context_manager(server):
    with DsServiceClient(server) as client:
        client.map_set("k", b"v")
        assert client.map_get("k") == b"v"


def test_context_manager_closes_the_channel(server):
    with DsServiceClient(server) as client:
        client.map_set("k", b"v")

    # grpc refuses calls on a closed channel
    # rather than quietly reconnecting,
    # so this is how the close is observed.
    with pytest.raises(ValueError, match="closed channel"):
        client.map_get("k")


def test_exception_in_the_block_still_closes_the_channel(server):
    with pytest.raises(RuntimeError):
        with DsServiceClient(server) as client:
            raise RuntimeError("boom")

    with pytest.raises(ValueError, match="closed channel"):
        client.map_get("k")


def test_close_is_still_callable_directly(server):
    client = DsServiceClient(server)
    client.map_set("k", b"v")
    client.close()

    with pytest.raises(ValueError, match="closed channel"):
        client.map_get("k")
