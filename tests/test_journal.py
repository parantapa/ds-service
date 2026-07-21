"""Tests for the key-to-journal store."""


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
