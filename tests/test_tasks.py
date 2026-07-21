"""Tests for the priority task queue."""

import time

import pytest

from ds_service_client import TaskState


def test_add_get_done_lifecycle(client):
    client.task_add("t1", queue="work", priority=1.0, function=b"fn", input=b"in")

    assert client.task_status("t1").state == TaskState.Ready

    task = client.task_get(worker_id="w1", queue="work")
    assert task.task_id == "t1"
    assert task.function == b"fn"
    assert task.input == b"in"
    assert client.task_status("t1").state == TaskState.Running

    client.task_done("t1", output=b"result")
    status = client.task_status("t1")
    assert status.state == TaskState.Complete
    assert status.output == b"result"


def test_get_from_empty_queue_raises_timeout(client):
    with pytest.raises(TimeoutError):
        client.task_get(worker_id="w1", queue="work")


def test_duplicate_add_raises_valueerror(client):
    client.task_add("dup", queue="work", priority=1.0, function=b"", input=b"")
    with pytest.raises(ValueError):
        client.task_add("dup", queue="work", priority=1.0, function=b"", input=b"")


def test_status_of_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_status("ghost")


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


def test_requeue_returns_stalled_task(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")  # now Running

    # No work left to hand out while the task is Running.
    with pytest.raises(TimeoutError):
        client.task_get(worker_id="w2", queue="work")

    # Reset any task running longer than the (tiny) timeout back to Ready.
    time.sleep(0.05)
    client.requeue(timeout_s=0.0)

    assert client.task_status("t").state == TaskState.Ready
    assert client.task_get(worker_id="w2", queue="work").task_id == "t"
