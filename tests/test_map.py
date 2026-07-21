"""Tests for the key-value map."""

import pytest


def test_set_then_get(client):
    client.map_set("greeting", b"hello")
    assert client.map_get("greeting") == b"hello"


def test_set_overwrites(client):
    client.map_set("k", b"first")
    client.map_set("k", b"second")
    assert client.map_get("k") == b"second"


def test_get_missing_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.map_get("does-not-exist")


def test_values_are_binary_safe(client):
    payload = bytes(range(256))
    client.map_set("blob", payload)
    assert client.map_get("blob") == payload
