"""Tests for the priority task queue."""

import pytest

from ds_service_client import NoTaskAvailable, TaskState, TaskStateError


def test_add_get_done_lifecycle(client):
    client.task_add("t1", queue="work", priority=1.0, function=b"fn", input=b"in")

    assert client.task_get_status("t1") == TaskState.Ready

    task = client.task_get(worker_id="w1", queue="work")
    assert task.task_id == "t1"
    assert task.function == b"fn"
    assert task.input == b"in"
    assert client.task_get_status("t1") == TaskState.Running

    client.task_done("t1", worker_id="w1", output=b"result")
    assert client.task_get_status("t1") == TaskState.Complete
    assert client.task_get_output("t1") == b"result"


def test_get_from_empty_queue_raises_no_task_available(client):
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w1", queue="work")


def test_no_task_available_is_not_a_timeout_error(client):
    # The distinction a worker loop depends on:
    # an idle queue must not look like an unreachable server,
    # which is what TimeoutError means.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w1", queue="work")

    try:
        client.task_get(worker_id="w1", queue="work")
    except NoTaskAvailable as exc:
        assert not isinstance(exc, TimeoutError)


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


def test_claimed_task_is_not_handed_out_again(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")  # now Running

    # A Running task stays with the worker that claimed it;
    # nothing hands it back to the queue.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue="work")

    assert client.task_get_status("t") == TaskState.Running


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
    client.task_done(claimed.task_id, worker_id="w2", output=b"out")

    counts = client.task_get_count_by_state()
    assert (counts.ready, counts.running, counts.complete) == (1, 1, 1)
    # The four counts always sum to the total number of tasks;
    # nothing here was cancelled, so that count is zero.
    assert counts.ready + counts.running + counts.complete + counts.canceled == 3


def test_equal_priority_is_dispatched_in_insertion_order(client):
    # Equal priorities used to come back in reverse insertion order,
    # because the heap broke ties on the row index, largest first.
    for name in ["a", "b", "c", "d"]:
        client.task_add(name, queue="work", priority=1.0, function=b"", input=b"")

    dispatched = [
        client.task_get(worker_id="w1", queue="work").task_id for _ in range(4)
    ]
    assert dispatched == ["a", "b", "c", "d"]


def test_priority_still_outranks_insertion_order(client):
    client.task_add("first", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("second", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("urgent", queue="work", priority=9.0, function=b"", input=b"")

    dispatched = [
        client.task_get(worker_id="w1", queue="work").task_id for _ in range(3)
    ]
    assert dispatched == ["urgent", "first", "second"]


def test_done_on_ready_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    # Never claimed, so there is no result to record.
    with pytest.raises(TaskStateError):
        client.task_done("t", worker_id="w1", output=b"result")

    assert client.task_get_status("t") == TaskState.Ready
    assert client.task_get_output("t") == b""


def test_done_twice_raises_the_second_time(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"first")

    with pytest.raises(TaskStateError):
        client.task_done("t", worker_id="w1", output=b"second")

    # The first result stands.
    assert client.task_get_output("t") == b"first"


def test_done_from_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_done("ghost", worker_id="w1", output=b"")


def test_done_from_another_worker_raises(client):
    """A task belongs to the worker that claimed it."""
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")

    # w2 never claimed the task, so it cannot record a result for it.
    with pytest.raises(TaskStateError):
        client.task_done("t", worker_id="w2", output=b"stale")

    assert client.task_get_status("t") == TaskState.Running

    # w1, the actual owner, still completes it.
    client.task_done("t", worker_id="w1", output=b"fresh")
    assert client.task_get_output("t") == b"fresh"


def test_get_priority_returns_what_task_add_set(client):
    client.task_add("t", queue="work", priority=2.5, function=b"", input=b"")

    assert client.task_get_priority("t") == 2.5


def test_set_priority_is_read_back(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    client.task_set_priority("t", 7.5)
    assert client.task_get_priority("t") == 7.5


def test_get_priority_of_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_get_priority("ghost")


def test_set_priority_of_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_set_priority("ghost", 1.0)


def test_raising_priority_moves_a_waiting_task_forward(client):
    client.task_add("a", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")

    client.task_set_priority("b", 9.0)

    dispatched = [
        client.task_get(worker_id="w1", queue="work").task_id for _ in range(2)
    ]
    assert dispatched == ["b", "a"]


def test_lowering_priority_moves_a_waiting_task_back(client):
    # The entry pushed by task_add outranks the new one,
    # so this only works if the older entry is retired rather than skipped.
    client.task_add("a", queue="work", priority=9.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")

    client.task_set_priority("a", 0.5)

    dispatched = [
        client.task_get(worker_id="w1", queue="work").task_id for _ in range(2)
    ]
    assert dispatched == ["b", "a"]


def test_reprioritized_task_goes_behind_its_new_equals(client):
    client.task_add("a", queue="work", priority=5.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("c", queue="work", priority=5.0, function=b"", input=b"")

    # b joins the 5.0 tasks, at the back of them rather than at its old place.
    client.task_set_priority("b", 5.0)

    dispatched = [
        client.task_get(worker_id="w1", queue="work").task_id for _ in range(3)
    ]
    assert dispatched == ["a", "c", "b"]


def test_repeated_set_priority_dispatches_the_task_once(client):
    """Every change retires the entries the previous one left behind."""
    client.task_add("t", queue=["alpha", "beta"], priority=1.0, function=b"", input=b"")

    for priority in [2.0, 3.0, 0.5]:
        client.task_set_priority("t", priority)

    assert client.task_get(worker_id="w1", queue=["alpha", "beta"]).task_id == "t"
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue=["alpha", "beta"])


def test_set_priority_on_a_running_task_does_not_queue_it_again(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")

    client.task_set_priority("t", 9.0)

    # The new priority is recorded, but the task stays with its worker.
    assert client.task_get_priority("t") == 9.0
    assert client.task_get_status("t") == TaskState.Running
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue="work")


def test_get_worker_id_of_a_running_task(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")

    assert client.task_get_worker_id("t") == "w1"


def test_get_worker_id_of_a_ready_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    # Nobody has claimed it, so there is no holder to name.
    with pytest.raises(TaskStateError):
        client.task_get_worker_id("t")


def test_get_worker_id_of_a_complete_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"result")

    # The task was handed back when it completed.
    with pytest.raises(TaskStateError):
        client.task_get_worker_id("t")


def test_get_worker_id_of_a_canceled_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_cancel("t")

    # Cancelling drops the record of who held it.
    with pytest.raises(TaskStateError):
        client.task_get_worker_id("t")


def test_get_worker_id_of_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_get_worker_id("ghost")


def test_cancel_a_ready_task(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    assert client.task_cancel("t") is True
    assert client.task_get_status("t") == TaskState.Canceled

    # The entry left behind in the queue is dead, like any other
    # entry whose task is no longer Ready.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w1", queue="work")


def test_cancel_a_running_task(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")

    assert client.task_cancel("t") is True
    assert client.task_get_status("t") == TaskState.Canceled

    # Cancelling does not put the task back on its queue.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue="work")


def test_cancel_a_complete_task_fails(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"result")

    # A finished task keeps its result; nothing was moved.
    assert client.task_cancel("t") is False
    assert client.task_get_status("t") == TaskState.Complete
    assert client.task_get_output("t") == b"result"


def test_cancel_twice_fails_the_second_time(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    assert client.task_cancel("t") is True
    # The second call did not move the task; the first one did.
    assert client.task_cancel("t") is False
    assert client.task_get_status("t") == TaskState.Canceled


def test_cancel_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_cancel("ghost")


def test_done_on_a_canceled_task_is_accepted_and_ignored(client):
    """A worker reporting work that was cancelled is not an error."""
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_cancel("t")

    # w1 finishes and reports, not knowing about the cancellation.
    client.task_done("t", worker_id="w1", output=b"result")

    # The call succeeded, but nothing was recorded.
    assert client.task_get_status("t") == TaskState.Canceled
    assert client.task_get_output("t") == b""


def test_count_by_state_counts_canceled_tasks(client):
    client.task_add("a", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")
    client.task_cancel("a")

    counts = client.task_get_count_by_state()
    assert (counts.ready, counts.running, counts.complete, counts.canceled) == (
        1,
        0,
        0,
        1,
    )
    # The four counts always sum to the total number of tasks.
    assert counts.ready + counts.running + counts.complete + counts.canceled == 2


def test_multi_queue_task_is_dispatched_once(client):
    """A task in several queues is handed out once, not once per queue."""
    client.task_add("t", queue=["alpha", "beta"], priority=1.0, function=b"", input=b"")

    assert client.task_get(worker_id="w1", queue=["alpha", "beta"]).task_id == "t"

    # The claim leaves an entry behind on the queue it was not taken from;
    # that entry is dead, because the task is no longer Ready.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue=["alpha", "beta"])


def test_search_id_matches_subset(client):
    for task_id in ["run/1", "run/2", "trial/1"]:
        client.task_add(task_id, queue="work", priority=1.0, function=b"", input=b"")

    assert sorted(client.task_search_id("^run/")) == ["run/1", "run/2"]


def test_search_id_is_unanchored(client):
    client.task_add(
        "study-alpha-1", queue="work", priority=1.0, function=b"", input=b""
    )
    client.task_add("study-beta-1", queue="work", priority=1.0, function=b"", input=b"")

    assert client.task_search_id("alpha") == ["study-alpha-1"]


def test_search_id_finds_tasks_in_every_state(client):
    for task_id in ["ready", "running", "complete", "canceled"]:
        client.task_add(task_id, queue=task_id, priority=1.0, function=b"", input=b"")

    client.task_get(worker_id="w1", queue="running")
    client.task_get(worker_id="w1", queue="complete")
    client.task_done("complete", worker_id="w1", output=b"")
    client.task_cancel("canceled")

    assert sorted(client.task_search_id(".*")) == [
        "canceled",
        "complete",
        "ready",
        "running",
    ]


def test_search_id_on_empty_store(client):
    assert client.task_search_id(".*") == []


def test_search_id_invalid_pattern_raises_valueerror(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    with pytest.raises(ValueError):
        client.task_search_id("(unclosed")


def test_reads_do_not_create_task(client):
    # None of these is allowed to add a row for the id it asks about.
    assert client.task_get_status("never-seen") == TaskState.Undefined
    for read in (
        client.task_get_output,
        client.task_get_priority,
        client.task_get_worker_id,
    ):
        with pytest.raises(KeyError):
            read("never-seen")

    assert client.task_search_id(".*") == []
