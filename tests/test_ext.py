"""Tests for the extension module ds_service_client._ext on its own.

The rest of the suite reaches _ext through the Python clients.
These tests pin down what the clients rely on it for.
"""

import re
from pathlib import Path

import pytest

from ds_service_client import _ext

PROTO = Path(__file__).resolve().parents[1] / "cpp" / "grpc" / "ds-service.proto"

# Every byte value, twice, including the ones UTF-8 cannot decode.
BINARY = bytes(range(256)) * 2


def test_task_state_matches_the_proto():
    match = re.search(r"enum TaskState \{(.*?)\}", PROTO.read_text(), flags=re.DOTALL)
    assert match, "TaskState not found in the proto"
    proto_values = {
        name: int(value)
        for name, value in re.findall(r"(\w+)\s*=\s*(\d+);", match.group(1))
    }
    assert {state.name: state.value for state in _ext.TaskState} == proto_values


def test_task_state_compares_as_an_int():
    assert _ext.TaskState.Ready == 1
    assert int(_ext.TaskState.Undefined) == 6


def test_client_error_carries_code_and_message():
    with pytest.raises(_ext.ClientError) as excinfo:
        _ext.connect("carrier-pigeon://127.0.0.1:1", 1.0)
    error = excinfo.value
    assert error.code == _ext.ErrorCode.InvalidArgument  # type: ignore[attr-defined]
    assert "carrier-pigeon" in error.message  # type: ignore[attr-defined]
    assert error.args == (error.code, error.message)  # type: ignore[attr-defined]


def test_error_thrown_with_the_gil_released_reaches_python(server):
    # Every call runs with the GIL released,
    # and the C++ exception is translated only once the GIL is back.
    client = _ext.connect(server, 5.0)
    with pytest.raises(_ext.ClientError) as excinfo:
        client.map_get("missing")
    assert excinfo.value.args[0] == _ext.ErrorCode.NotFound
    client.close()


def test_payloads_hold_arbitrary_bytes(server):
    client = _ext.connect(server, 5.0)
    try:
        client.map_set("k", BINARY)
        assert client.map_get("k") == BINARY

        client.journal_append("j", BINARY)
        assert client.journal_read("j", 0, 1) == [BINARY]

        client.task_add("t", [], ["q"], 1.0, BINARY, BINARY[::-1])
        task = client.task_get("w", ["q"])
        assert isinstance(task.function, bytes)
        assert (task.task_id, task.function, task.input) == ("t", BINARY, BINARY[::-1])
        client.task_done("t", "w", BINARY, False)
        assert client.task_get_output("t") == BINARY
    finally:
        client.close()


def test_payload_arguments_must_be_bytes(server):
    client = _ext.connect(server, 5.0)
    try:
        with pytest.raises(TypeError):
            client.map_set("k", "not bytes")  # type: ignore[arg-type]
    finally:
        client.close()


def test_coexists_with_grpcio(server):
    # The module links its own copy of gRPC with hidden symbols,
    # so a process can load grpcio beside it and use both.
    grpc = pytest.importorskip("grpc")

    channel = grpc.insecure_channel(server)
    try:
        grpc.channel_ready_future(channel).result(timeout=10)
    finally:
        channel.close()

    client = _ext.connect(server, 5.0)
    try:
        client.map_set("k", b"v")
        assert client.map_get("k") == b"v"
    finally:
        client.close()
