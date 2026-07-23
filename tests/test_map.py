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


def test_search_key_matches_subset(client):
    for key in ["run/1", "run/2", "trial/1"]:
        client.map_set(key, b"v")

    assert sorted(client.map_search_key("^run/")) == ["run/1", "run/2"]


def test_search_key_is_unanchored(client):
    client.map_set("study-alpha-1", b"v")
    client.map_set("study-beta-1", b"v")

    assert client.map_search_key("alpha") == ["study-alpha-1"]


def test_search_key_full_match_needs_anchors(client):
    client.map_set("abc", b"v")
    client.map_set("xabcx", b"v")

    assert sorted(client.map_search_key("abc")) == ["abc", "xabcx"]
    assert client.map_search_key("^abc$") == ["abc"]


def test_search_key_no_match_returns_empty(client):
    client.map_set("k", b"v")
    assert client.map_search_key("^nothing-matches-this$") == []


def test_search_key_on_empty_map(client):
    assert client.map_search_key(".*") == []


def test_search_key_matches_everything(client):
    for key in ["a", "b", "c"]:
        client.map_set(key, b"v")

    assert sorted(client.map_search_key(".*")) == ["a", "b", "c"]


def test_search_key_invalid_pattern_raises_valueerror(client):
    with pytest.raises(ValueError):
        client.map_search_key("(unclosed")


def test_search_key_invalid_pattern_leaves_server_usable(client):
    client.map_set("k", b"v")

    with pytest.raises(ValueError):
        client.map_search_key("*bad")

    assert client.map_get("k") == b"v"
