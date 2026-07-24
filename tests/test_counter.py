"""Tests for the counter data structure."""

import pytest


def test_first_value_is_one(client):
    assert client.counter_get_next_value("c") == 1


def test_values_increment(client):
    assert [client.counter_get_next_value("c") for _ in range(5)] == [1, 2, 3, 4, 5]


def test_counters_are_independent(client):
    assert client.counter_get_next_value("a") == 1
    assert client.counter_get_next_value("b") == 1
    assert client.counter_get_next_value("a") == 2
    assert client.counter_get_next_value("b") == 2
    assert client.counter_get_next_value("a") == 3


def test_counter_is_created_on_first_call(client):
    # A key never seen before starts its own sequence at 1.
    assert client.counter_get_next_value("fresh") == 1
    assert client.counter_get_next_value("fresh") == 2


def test_search_key_matches_subset(client):
    for key in ["run/1", "run/2", "trial/1"]:
        client.counter_get_next_value(key)

    assert sorted(client.counter_search_key("^run/")) == ["run/1", "run/2"]


def test_search_key_is_unanchored(client):
    client.counter_get_next_value("study-alpha-1")
    client.counter_get_next_value("study-beta-1")

    assert client.counter_search_key("alpha") == ["study-alpha-1"]


def test_search_key_on_empty_store(client):
    assert client.counter_search_key(".*") == []


def test_search_key_invalid_pattern_raises_valueerror(client):
    client.counter_get_next_value("k")

    with pytest.raises(ValueError):
        client.counter_search_key("(unclosed")
