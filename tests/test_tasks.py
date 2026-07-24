"""Tests for the priority task queue."""

import time

import pytest

from ds_service_client import TaskState


def test_add_get_done_lifecycle(client):
    client.task_add("t1", queue="work", priority=1.0, function=b"fn", input=b"in")

    assert client.task_get_status("t1") == TaskState.Ready

    task = client.task_get(worker_id="w1", queue="work")
    assert task.task_id == "t1"
    assert task.function == b"fn"
    assert task.input == b"in"
    assert client.task_get_status("t1") == TaskState.Running

    client.task_done("t1", output=b"result")
    assert client.task_get_status("t1") == TaskState.Complete
    assert client.task_get_output("t1") == b"result"


def test_get_from_empty_queue_raises_timeout(client):
    with pytest.raises(TimeoutError):
        client.task_get(worker_id="w1", queue="work")


def test_duplicate_add_raises_valueerror(client):
    client.task_add("dup", queue="work", priority=1.0, function=b"", input=b"")
    with pytest.raises(ValueError):
        client.task_add("dup", queue="work", priority=1.0, function=b"", input=b"")


def test_status_of_unknown_task_is_undefined(client):
    # An unknown task_id reports Undefined rather than raising.
    assert client.task_get_status("ghost") == TaskState.Undefined


def test_get_status_accepts_many_ids_in_order(client):
    client.task_add("a", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")  # claims the higher/earlier one

    # States come back positionally, and a missing id fills in Undefined.
    states = client.task_get_status(["a", "ghost", "b"])
    assert states[1] == TaskState.Undefined
    assert {states[0], states[2]} == {TaskState.Ready, TaskState.Running}


def test_get_status_of_empty_list_is_empty(client):
    assert client.task_get_status([]) == []


def test_get_status_return_shape_follows_input(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    # A single string returns a bare TaskState, not a list.
    assert client.task_get_status("t") == TaskState.Ready
    assert not isinstance(client.task_get_status("t"), list)

    # A one-element list returns a one-element list.
    assert client.task_get_status(["t"]) == [TaskState.Ready]


def test_output_of_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_get_output("ghost")


def test_output_before_done_is_empty(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    # The task exists but has produced no output yet.
    assert client.task_get_output("t") == b""


def test_higher_priority_is_dispatched_first(client):
    client.task_add("low", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("high", queue="work", priority=5.0, function=b"", input=b"")

    assert client.task_get(worker_id="w1", queue="work").task_id == "high"
    assert client.task_get(worker_id="w1", queue="work").task_id == "low"


def test_task_dispatched_on_any_of_its_queues(client):
    client.task_add("t", queue=["alpha", "beta"], priority=1.0, function=b"", input=b"")
    # Nothing on alpha's dispatch means the worker falls through to beta.
    task = client.task_get(worker_id="w1", queue=["empty", "beta"])
    assert task.task_id == "t"


def test_worker_polls_across_queues_in_order(client):
    client.task_add("a", queue="qa", priority=1.0, function=b"", input=b"")
    client.task_add("b", queue="qb", priority=1.0, function=b"", input=b"")

    # First non-empty queue in the request order wins.
    assert client.task_get(worker_id="w1", queue=["qa", "qb"]).task_id == "a"
    assert client.task_get(worker_id="w1", queue=["qa", "qb"]).task_id == "b"


def test_task_requeue_returns_stalled_task(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")  # now Running

    # No work left to hand out while the task is Running.
    with pytest.raises(TimeoutError):
        client.task_get(worker_id="w2", queue="work")

    # Reset any task running longer than the (tiny) timeout back to Ready.
    time.sleep(0.05)
    client.task_requeue(timeout_s=0.0)

    assert client.task_get_status("t") == TaskState.Ready
    assert client.task_get(worker_id="w2", queue="work").task_id == "t"


def test_count_by_state_on_empty_system_is_zero(client):
    counts = client.task_get_count_by_state()
    assert (counts.ready, counts.running, counts.complete) == (0, 0, 0)


def test_count_by_state_tracks_lifecycle(client):
    client.task_add("a", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("c", queue="work", priority=1.0, function=b"", input=b"")

    # All three start Ready.
    counts = client.task_get_count_by_state()
    assert (counts.ready, counts.running, counts.complete) == (3, 0, 0)

    # Claim one (Ready -> Running) and complete another.
    client.task_get(worker_id="w1", queue="work")
    claimed = client.task_get(worker_id="w2", queue="work")
    client.task_done(claimed.task_id, output=b"out")

    counts = client.task_get_count_by_state()
    assert (counts.ready, counts.running, counts.complete) == (1, 1, 1)
    # The three counts always sum to the total number of tasks.
    assert counts.ready + counts.running + counts.complete == 3
