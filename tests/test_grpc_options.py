"""Tests for the gRPC channel options on both sides of the wire.

See "The channel settings are one setting in two languages"
in docs/developer-notes.md for why the two sides must match.
"""

import re
import shlex
import subprocess
from pathlib import Path

import grpc
import pytest

from ds_service_client.client import GRPC_CLIENT_OPTIONS, MAX_MESSAGE_SIZE_BYTES

SERVER_SOURCE = Path(__file__).resolve().parents[1] / "cpp" / "ds-service.cpp"


def _server_source() -> str:
    """The server source with C-style comments stripped."""
    # The server's channel arguments carry inline /* ... */ notes,
    # which otherwise confuse the arithmetic these tests evaluate.
    return re.sub(r"/\*.*?\*/", "", SERVER_SOURCE.read_text(), flags=re.DOTALL)


def _client_option(name: str) -> int:
    return dict(GRPC_CLIENT_OPTIONS)[name]


def test_message_size_limits_match_the_server():
    match = re.search(
        r"constexpr int MAX_MESSAGE_SIZE_BYTES\s*=\s*([^;]+);", _server_source()
    )
    assert match, "MAX_MESSAGE_SIZE_BYTES not found in the server source"
    assert eval(match.group(1)) == MAX_MESSAGE_SIZE_BYTES


def test_client_ping_interval_clears_the_server_floor():
    match = re.search(
        r"GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,\s*([^)]+)\)",
        _server_source(),
    )
    assert match, "server ping floor not found in the server source"
    floor_ms = eval(match.group(1))
    assert _client_option("grpc.keepalive_time_ms") > floor_ms


def test_idle_keepalive_is_not_capped():
    # A finite cap is a total, not a rate.
    # The client then stops pinging on a long-idle connection,
    # which is the one keepalive is there to protect.
    assert _client_option("grpc.http2.max_pings_without_data") == 0
    assert _client_option("grpc.keepalive_permit_without_calls") == 1


def test_large_value_round_trips(client):
    # Comfortably past gRPC's own 4 MiB default, which used to be the ceiling.
    payload = b"x" * (8 * 1024 * 1024)
    client.map_set("big", payload)
    assert client.map_get("big") == payload


def test_oversized_value_raises_value_error(client):
    payload = b"x" * (MAX_MESSAGE_SIZE_BYTES + 1024)
    with pytest.raises(ValueError) as excinfo:
        client.map_set("too-big", payload)
    assert not isinstance(excinfo.value, grpc.RpcError)


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
