"""Tests for the key-to-journal store."""

import pytest


def test_size_of_missing_journal_is_zero(client):
    assert client.journal_size("nope") == 0


def test_read_of_missing_journal_is_empty(client):
    assert client.journal_read("nope", 0, 5) == []


def test_append_creates_and_grows(client):
    client.journal_append("j", b"a")
    client.journal_append("j", b"b")
    client.journal_append("j", b"c")
    assert client.journal_size("j") == 3


def test_read_is_half_open(client):
    for entry in (b"a", b"b", b"c"):
        client.journal_append("j", entry)
    assert client.journal_read("j", 0, 2) == [b"a", b"b"]


def test_full_read_via_size(client):
    for entry in (b"a", b"b", b"c"):
        client.journal_append("j", entry)
    assert client.journal_read("j", 0, client.journal_size("j")) == [b"a", b"b", b"c"]


def test_read_clamps_end_past_size(client):
    for entry in (b"a", b"b", b"c"):
        client.journal_append("j", entry)
    assert client.journal_read("j", 1, 100) == [b"b", b"c"]


def test_read_start_past_size_is_empty(client):
    for entry in (b"a", b"b", b"c"):
        client.journal_append("j", entry)
    assert client.journal_read("j", 50, 60) == []


def test_read_with_start_ge_end_is_empty(client):
    for entry in (b"a", b"b", b"c"):
        client.journal_append("j", entry)
    assert client.journal_read("j", 2, 2) == []
    assert client.journal_read("j", 3, 1) == []


def test_entries_are_binary_safe(client):
    payload = b"\x00\x01\x02\xff"
    client.journal_append("j", payload)
    assert client.journal_read("j", 0, 1) == [payload]


def test_journals_are_independent(client):
    client.journal_append("a", b"x")
    client.journal_append("b", b"y")
    client.journal_append("b", b"z")
    assert client.journal_size("a") == 1
    assert client.journal_size("b") == 2
    assert client.journal_read("a", 0, 10) == [b"x"]


def test_search_key_matches_subset(client):
    for key in ["run/1", "run/2", "trial/1"]:
        client.journal_append(key, b"v")

    assert sorted(client.journal_search_key("^run/")) == ["run/1", "run/2"]


def test_search_key_is_unanchored(client):
    client.journal_append("study-alpha-1", b"v")
    client.journal_append("study-beta-1", b"v")

    assert client.journal_search_key("alpha") == ["study-alpha-1"]


def test_search_key_on_empty_store(client):
    assert client.journal_search_key(".*") == []


def test_search_key_invalid_pattern_raises_valueerror(client):
    client.journal_append("k", b"v")

    with pytest.raises(ValueError):
        client.journal_search_key("(unclosed")
