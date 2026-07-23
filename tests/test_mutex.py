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
