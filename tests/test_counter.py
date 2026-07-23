"""Tests for the counter data structure."""


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
