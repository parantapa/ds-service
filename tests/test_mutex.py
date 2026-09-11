"""Tests for the mutex data structure."""

import threading
import time

import pytest

from ds_service_client import MutexNotHeld


def test_try_acquire_creates_and_acquires(client):
    # The first call creates and acquires a mutex that does not exist.
    assert client.mutex_try_acquire("m", worker_id="w1") is True


def test_try_acquire_second_time_fails(client):
    assert client.mutex_try_acquire("m", worker_id="w1") is True
    assert client.mutex_try_acquire("m", worker_id="w2") is False


def test_try_acquire_is_not_reentrant(client):
    # The holder gets no special treatment:
    # a mutex it already holds is held,
    # and a second call does not acquire it again.
    assert client.mutex_try_acquire("m", worker_id="w1") is True
    assert client.mutex_try_acquire("m", worker_id="w1") is False


def test_release_allows_reacquire(client):
    assert client.mutex_try_acquire("m", worker_id="w1") is True
    client.mutex_release("m", worker_id="w1")
    assert client.mutex_try_acquire("m", worker_id="w2") is True


def test_release_by_another_worker_raises(client):
    assert client.mutex_try_acquire("m", worker_id="w1") is True

    # w2 never held it, so it cannot hand w1's exclusive section to anybody.
    with pytest.raises(MutexNotHeld):
        client.mutex_release("m", worker_id="w2")

    # w1 still holds it, and can still release it.
    assert client.mutex_try_acquire("m", worker_id="w2") is False
    client.mutex_release("m", worker_id="w1")
    assert client.mutex_try_acquire("m", worker_id="w2") is True


def test_release_clears_the_holder(client):
    client.mutex_try_acquire("m", worker_id="w1")
    client.mutex_release("m", worker_id="w1")
    client.mutex_try_acquire("m", worker_id="w2")

    # The mutex belongs to w2 now.
    # w1's claim on it is gone.
    with pytest.raises(MutexNotHeld):
        client.mutex_release("m", worker_id="w1")


def test_release_of_a_free_mutex_raises(client):
    client.mutex_try_acquire("m", worker_id="w1")
    client.mutex_release("m", worker_id="w1")

    # Nobody holds it, so there is no holder to match.
    with pytest.raises(MutexNotHeld):
        client.mutex_release("m", worker_id="w1")

    assert client.mutex_try_acquire("m", worker_id="w1") is True


def test_release_of_unknown_mutex_raises(client):
    with pytest.raises(MutexNotHeld):
        client.mutex_release("never-created", worker_id="w1")

    assert client.mutex_try_acquire("never-created", worker_id="w1") is True


def test_acquire_returns_immediately_when_free(client):
    start = time.monotonic()
    client.mutex_acquire("m", worker_id="w1")
    assert time.monotonic() - start < 0.4  # returned without a retry sleep
    assert client.mutex_try_acquire("m", worker_id="w2") is False  # w1 holds it


def test_acquire_times_out_when_held(client):
    assert client.mutex_try_acquire("m", worker_id="w1") is True
    with pytest.raises(TimeoutError):
        client.mutex_acquire("m", worker_id="w2", timeout=0.3)


def test_acquire_unblocks_after_release(client):
    assert client.mutex_try_acquire("m", worker_id="w1") is True

    def releaser():
        time.sleep(0.6)
        client.mutex_release("m", worker_id="w1")

    t = threading.Thread(target=releaser)
    t.start()
    try:
        client.mutex_acquire("m", worker_id="w2", timeout=5.0)  # once released
    finally:
        t.join()
    assert client.mutex_try_acquire("m", worker_id="w1") is False  # w2 holds it


def test_get_worker_id_names_the_holder(client):
    client.mutex_try_acquire("m", worker_id="w1")

    assert client.mutex_get_worker_id("m") == "w1"

    # A failed acquire does not change who holds it.
    assert client.mutex_try_acquire("m", worker_id="w2") is False
    assert client.mutex_get_worker_id("m") == "w1"


def test_get_worker_id_of_a_free_mutex_raises(client):
    client.mutex_try_acquire("m", worker_id="w1")
    client.mutex_release("m", worker_id="w1")

    # The key still exists, but nobody holds it.
    with pytest.raises(MutexNotHeld):
        client.mutex_get_worker_id("m")


def test_get_worker_id_of_unknown_mutex_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.mutex_get_worker_id("never-created")

    # A failed read must not create the key.
    assert client.mutex_search_key(".*") == []


def test_get_worker_id_follows_the_holder(client):
    client.mutex_try_acquire("m", worker_id="w1")
    client.mutex_release("m", worker_id="w1")
    client.mutex_try_acquire("m", worker_id="w2")

    assert client.mutex_get_worker_id("m") == "w2"


def test_empty_worker_id_is_a_holder_like_any_other(client):
    # The server tracks "held" separately from the holder's name.
    # A worker that calls itself "" holds the mutex,
    # and the mutex does not look free to everybody else.
    assert client.mutex_try_acquire("m", worker_id="") is True
    assert client.mutex_try_acquire("m", worker_id="") is False
    assert client.mutex_try_acquire("m", worker_id="w1") is False

    with pytest.raises(MutexNotHeld):
        client.mutex_release("m", worker_id="w1")

    client.mutex_release("m", worker_id="")
    assert client.mutex_try_acquire("m", worker_id="w1") is True


def test_search_key_matches_subset(client):
    for key in ["run/1", "run/2", "trial/1"]:
        client.mutex_try_acquire(key, worker_id="w1")

    assert sorted(client.mutex_search_key("^run/")) == ["run/1", "run/2"]


def test_search_key_is_unanchored(client):
    client.mutex_try_acquire("study-alpha-1", worker_id="w1")
    client.mutex_try_acquire("study-beta-1", worker_id="w1")

    assert client.mutex_search_key("alpha") == ["study-alpha-1"]


def test_search_key_finds_released_keys(client):
    # A released mutex still exists as a (free) key.
    client.mutex_try_acquire("held", worker_id="w1")
    client.mutex_try_acquire("freed", worker_id="w1")
    client.mutex_release("freed", worker_id="w1")

    assert sorted(client.mutex_search_key(".*")) == ["freed", "held"]


def test_search_key_finds_a_key_that_was_never_acquired(client):
    # A failed try_acquire still names the key, which creates it.
    client.mutex_try_acquire("contended", worker_id="w1")
    assert client.mutex_try_acquire("contended", worker_id="w2") is False

    assert client.mutex_search_key(".*") == ["contended"]


def test_search_key_on_empty_store(client):
    assert client.mutex_search_key(".*") == []


def test_search_key_invalid_pattern_raises_valueerror(client):
    client.mutex_try_acquire("k", worker_id="w1")

    with pytest.raises(ValueError):
        client.mutex_search_key("(unclosed")


def test_release_does_not_create_mutex(client):
    # A refused release must not leave the key behind.
    with pytest.raises(MutexNotHeld):
        client.mutex_release("never-seen", worker_id="w1")

    assert client.mutex_search_key(".*") == []
