"""Tests for the mutex data structure."""

import threading
import time

import pytest

from ds_service_client import client as client_module


def test_try_acquire_creates_and_acquires(client):
    # A mutex that does not exist is created and acquired by the first call.
    assert client.mutex_try_acquire("m") is True


def test_try_acquire_second_time_fails(client):
    assert client.mutex_try_acquire("m") is True
    assert client.mutex_try_acquire("m") is False


def test_release_allows_reacquire(client):
    assert client.mutex_try_acquire("m") is True
    client.mutex_release("m")
    assert client.mutex_try_acquire("m") is True


def test_release_unheld_is_noop(client):
    assert client.mutex_try_acquire("m") is True
    client.mutex_release("m")
    client.mutex_release("m")  # releasing an already-released mutex
    assert client.mutex_try_acquire("m") is True


def test_release_unknown_is_noop(client):
    client.mutex_release("never-created")  # must not raise
    assert client.mutex_try_acquire("never-created") is True


def test_acquire_returns_immediately_when_free(client):
    start = time.monotonic()
    client.mutex_acquire("m")
    assert time.monotonic() - start < 0.4  # returned without a retry sleep
    assert client.mutex_try_acquire("m") is False  # and we now hold it


def test_acquire_times_out_when_held(client):
    assert client.mutex_try_acquire("m") is True
    with pytest.raises(TimeoutError):
        client.mutex_acquire("m", timeout=0.3)


def test_acquire_unblocks_after_release(client):
    assert client.mutex_try_acquire("m") is True

    def releaser():
        time.sleep(0.6)
        client.mutex_release("m")

    t = threading.Thread(target=releaser)
    t.start()
    try:
        client.mutex_acquire("m", timeout=5.0)  # acquires once released
    finally:
        t.join()
    assert client.mutex_try_acquire("m") is False  # we hold it now


def test_retry_constants_are_defined():
    assert client_module.MUTEX_ACQUIRE_SLEEP_S == 0.5
    assert client_module.MUTEX_ACQUIRE_JITTER_S == 0.1


def test_search_key_matches_subset(client):
    for key in ["run/1", "run/2", "trial/1"]:
        client.mutex_try_acquire(key)

    assert sorted(client.mutex_search_key("^run/")) == ["run/1", "run/2"]


def test_search_key_is_unanchored(client):
    client.mutex_try_acquire("study-alpha-1")
    client.mutex_try_acquire("study-beta-1")

    assert client.mutex_search_key("alpha") == ["study-alpha-1"]


def test_search_key_finds_released_keys(client):
    # A released mutex still exists as a (free) key.
    client.mutex_try_acquire("held")
    client.mutex_try_acquire("freed")
    client.mutex_release("freed")

    assert sorted(client.mutex_search_key(".*")) == ["freed", "held"]


def test_search_key_on_empty_store(client):
    assert client.mutex_search_key(".*") == []


def test_search_key_invalid_pattern_raises_valueerror(client):
    client.mutex_try_acquire("k")

    with pytest.raises(ValueError):
        client.mutex_search_key("(unclosed")
