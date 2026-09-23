"""Tests for the gRPC channel settings, seen from a client.

These tests check only what a caller can observe.
Compile-time checks cover the rest.
See "The channel settings live in one header"
in docs/developer-notes.md.
"""

import shlex
import subprocess

import pytest

from ds_service_client import TransportError

# MAX_MESSAGE_SIZE_BYTES in cpp/grpc/channel-settings.hpp.
MAX_MESSAGE_SIZE_BYTES = 64 * 1024 * 1024


def test_large_value_round_trips(client):
    # Comfortably past gRPC's own 4 MiB default, which used to be the ceiling.
    payload = b"x" * (8 * 1024 * 1024)
    client.map_set("big", payload)
    assert client.map_get("big") == payload


def test_oversized_value_raises_value_error(client):
    payload = b"x" * (MAX_MESSAGE_SIZE_BYTES + 1024)
    with pytest.raises(ValueError) as excinfo:
        client.map_set("too-big", payload)
    assert not isinstance(excinfo.value, TransportError)


def test_second_server_on_the_same_port_fails(server, server_binary):
    # See "The server refuses to share its port" in the developer notes.
    # server_binary can be a whole command line, not only a path,
    # so this test splits it the way DsServiceServer does.
    second = subprocess.run(
        shlex.split(f"{server_binary} --address {server}"),
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert second.returncode != 0
    assert "Failed to bind" in second.stdout + second.stderr
