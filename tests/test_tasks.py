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
    assert client.task_get_status("t1") == TaskState.Finished
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

    assert client.task_get_status(["t"]) == [TaskState.Ready]


def test_output_of_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_get_output("ghost")


def test_output_before_done_is_empty(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    # The task exists, but has no output yet.
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

    # A Running task stays with the worker that claimed it.
    # Nothing hands it back to the queue.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue="work")

    assert client.task_get_status("t") == TaskState.Running


def test_count_by_state_on_empty_system_is_zero(client):
    counts = client.task_get_count_by_state()
    assert (counts.waiting, counts.ready, counts.running) == (0, 0, 0)


def test_count_by_state_tracks_lifecycle(client):
    client.task_add("a", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("c", queue="work", priority=1.0, function=b"", input=b"")

    # All three start Ready.
    counts = client.task_get_count_by_state()
    assert (counts.waiting, counts.ready, counts.running) == (0, 3, 0)

    client.task_get(worker_id="w1", queue="work")
    claimed = client.task_get(worker_id="w2", queue="work")
    client.task_done(claimed.task_id, worker_id="w2", output=b"out")

    counts = client.task_get_count_by_state()
    assert (counts.ready, counts.running, counts.finished) == (1, 1, 1)
    # The six counts always sum to the total number of tasks.
    # Nothing here waited, was canceled or failed, so those counts are zero.
    assert (
        counts.waiting
        + counts.ready
        + counts.running
        + counts.finished
        + counts.failed
        + counts.canceled
        == 3
    )


def test_equal_priority_is_dispatched_in_insertion_order(client):
    # Equal priorities once came back in reverse insertion order,
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

    # w1, the actual owner, still finishes it.
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
    # so this only works if the server retires the older entry
    # rather than skipping it.
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

    # The server records the new priority, but the task stays with its worker.
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

    # Nobody claimed it, so there is no holder to name.
    with pytest.raises(TaskStateError):
        client.task_get_worker_id("t")


def test_get_worker_id_of_a_finished_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"result")

    # The server released the task when it finished.
    with pytest.raises(TaskStateError):
        client.task_get_worker_id("t")


def test_get_worker_id_of_a_canceled_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_cancel("t")

    # Canceling drops the record of who held it.
    with pytest.raises(TaskStateError):
        client.task_get_worker_id("t")


def test_get_worker_id_of_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_get_worker_id("ghost")


def test_cancel_a_ready_task(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    assert client.task_cancel("t") is True
    assert client.task_get_status("t") == TaskState.Canceled

    # The entry left behind in the queue is dead,
    # like any other entry whose task is no longer Ready.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w1", queue="work")


def test_cancel_a_running_task(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")

    assert client.task_cancel("t") is True
    assert client.task_get_status("t") == TaskState.Canceled

    # Canceling does not put the task back on its queue.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue="work")


def test_cancel_a_finished_task_fails(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"result")

    # A finished task keeps its result.
    # Nothing was moved.
    assert client.task_cancel("t") is False
    assert client.task_get_status("t") == TaskState.Finished
    assert client.task_get_output("t") == b"result"


def test_cancel_twice_fails_the_second_time(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    assert client.task_cancel("t") is True
    # The second call did not move the task.
    # The first one did.
    assert client.task_cancel("t") is False
    assert client.task_get_status("t") == TaskState.Canceled


def test_cancel_unknown_task_raises_keyerror(client):
    with pytest.raises(KeyError):
        client.task_cancel("ghost")


def test_done_on_a_canceled_task_is_accepted_and_ignored(client):
    """A worker reporting work that was canceled is not an error."""
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_cancel("t")

    # w1 finishes and reports, not knowing about the cancellation.
    client.task_done("t", worker_id="w1", output=b"result")

    # The call succeeded, but the server recorded nothing of the result.
    assert client.task_get_status("t") == TaskState.Canceled
    assert client.task_get_output("t") == b"Task canceled"


def test_count_by_state_counts_canceled_tasks(client):
    client.task_add("a", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("b", queue="work", priority=1.0, function=b"", input=b"")
    client.task_cancel("a")

    counts = client.task_get_count_by_state()
    assert (
        counts.waiting,
        counts.ready,
        counts.running,
        counts.finished,
        counts.failed,
        counts.canceled,
    ) == (0, 1, 0, 0, 0, 1)
    # The six counts always sum to the total number of tasks.
    assert (
        counts.waiting
        + counts.ready
        + counts.running
        + counts.finished
        + counts.failed
        + counts.canceled
        == 2
    )


def test_multi_queue_task_is_dispatched_once(client):
    """The server hands out a task in several queues once, not once per queue."""
    client.task_add("t", queue=["alpha", "beta"], priority=1.0, function=b"", input=b"")

    assert client.task_get(worker_id="w1", queue=["alpha", "beta"]).task_id == "t"

    # The claim leaves an entry behind on the queue it was not taken from.
    # That entry is dead, because the task is no longer Ready.
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
    for task_id in ["ready", "running", "finished", "canceled"]:
        client.task_add(task_id, queue=task_id, priority=1.0, function=b"", input=b"")

    client.task_get(worker_id="w1", queue="running")
    client.task_get(worker_id="w1", queue="finished")
    client.task_done("finished", worker_id="w1", output=b"")
    client.task_cancel("canceled")

    assert sorted(client.task_search_id(".*")) == [
        "canceled",
        "finished",
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
    # None of these reads adds a row for the id it asks about.
    assert client.task_get_status("never-seen") == TaskState.Undefined
    for read in (
        client.task_get_output,
        client.task_get_priority,
        client.task_get_worker_id,
    ):
        with pytest.raises(KeyError):
            read("never-seen")

    assert client.task_search_id(".*") == []


def test_task_with_an_unfinished_parent_starts_waiting(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    assert client.task_get_status("child") == TaskState.Waiting

    # No queue offers a Waiting task, so only the parent is dispatched.
    assert client.task_get(worker_id="w1", queue="work").task_id == "parent"
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue="work")


def test_completing_the_parent_releases_the_child(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"")

    assert client.task_get_status("child") == TaskState.Ready
    assert client.task_get(worker_id="w1", queue="work").task_id == "child"


def test_a_child_waits_for_every_parent(client):
    for task_id in ["a", "b"]:
        client.task_add(task_id, queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids=["a", "b"],
    )

    client.task_get(worker_id="w1", queue="work")
    client.task_done("a", worker_id="w1", output=b"")
    assert client.task_get_status("child") == TaskState.Waiting

    client.task_get(worker_id="w1", queue="work")
    client.task_done("b", worker_id="w1", output=b"")
    assert client.task_get_status("child") == TaskState.Ready


def test_a_child_of_two_parents_is_dispatched_once(client):
    # The child enters its queue when the last parent finishes,
    # once, not once per parent.
    for task_id in ["a", "b"]:
        client.task_add(task_id, queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids=["a", "b"],
    )

    for task_id in ["a", "b"]:
        client.task_get(worker_id="w1", queue="work")
        client.task_done(task_id, worker_id="w1", output=b"")

    assert client.task_get(worker_id="w1", queue="work").task_id == "child"
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w2", queue="work")


def test_naming_a_parent_twice_still_releases_the_child(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids=["parent", "parent"],
    )

    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"")

    assert client.task_get_status("child") == TaskState.Ready


def test_a_finished_parent_leaves_the_child_ready(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"")

    # Nothing is left to wait for, so the child never passes through Waiting.
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    assert client.task_get_status("child") == TaskState.Ready


def test_unknown_parent_raises_keyerror_and_adds_nothing(client):
    with pytest.raises(KeyError):
        client.task_add(
            "child",
            queue="work",
            priority=1.0,
            function=b"",
            input=b"",
            parent_task_ids="ghost",
        )

    assert client.task_get_status("child") == TaskState.Undefined


def test_a_task_cannot_be_its_own_parent(client):
    # The row does not exist while its own TaskAdd is being served,
    # so naming itself is naming an unknown parent.
    # That is what keeps the dependency graph free of cycles.
    with pytest.raises(KeyError):
        client.task_add(
            "self",
            queue="work",
            priority=1.0,
            function=b"",
            input=b"",
            parent_task_ids="self",
        )


def test_canceling_a_parent_cancels_the_tasks_waiting_on_it(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )
    client.task_add(
        "grandchild",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="child",
    )

    assert client.task_cancel("parent") is True

    # The cancellation runs the whole way down the graph.
    assert client.task_get_status("child") == TaskState.Canceled
    assert client.task_get_status("grandchild") == TaskState.Canceled


def test_a_child_of_a_canceled_parent_is_added_canceled(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_cancel("parent")

    # A canceled parent never finishes,
    # so the child is canceled rather than left waiting for it.
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    assert client.task_get_status("child") == TaskState.Canceled


def test_a_waiting_task_can_be_canceled(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    assert client.task_cancel("child") is True
    assert client.task_get_status("child") == TaskState.Canceled

    # Completing the parent does not bring the canceled child back.
    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"")
    assert client.task_get_status("child") == TaskState.Canceled
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w1", queue="work")


def test_priority_set_while_waiting_applies_when_the_task_is_released(client):
    client.task_add("parent", queue="work", priority=5.0, function=b"", input=b"")
    for task_id in ["low", "high"]:
        client.task_add(
            task_id,
            queue="work",
            priority=1.0,
            function=b"",
            input=b"",
            parent_task_ids="parent",
        )
    client.task_set_priority("high", 9.0)

    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"")

    # Both entered the queue together, so the new priority decides the order.
    assert client.task_get(worker_id="w1", queue="work").task_id == "high"
    assert client.task_get(worker_id="w1", queue="work").task_id == "low"


def test_a_released_task_goes_behind_its_equals(client):
    client.task_add("parent", queue="work", priority=5.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )
    client.task_add("other", queue="work", priority=1.0, function=b"", input=b"")

    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"")

    # A released task counts as newly queued,
    # so it waits behind the tasks of equal priority already there,
    # even though it was added first.
    assert client.task_get(worker_id="w1", queue="work").task_id == "other"
    assert client.task_get(worker_id="w1", queue="work").task_id == "child"


def test_count_by_state_counts_waiting_tasks(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    counts = client.task_get_count_by_state()
    assert (
        counts.waiting,
        counts.ready,
        counts.running,
        counts.finished,
        counts.failed,
        counts.canceled,
    ) == (1, 1, 0, 0, 0, 0)


def test_done_with_failed_marks_the_task_failed(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")

    client.task_done("t", worker_id="w1", output=b"traceback", failed=True)

    # A task that ran keeps whatever its worker reported.
    assert client.task_get_status("t") == TaskState.Failed
    assert client.task_get_output("t") == b"traceback"


def test_a_canceled_task_reports_its_output(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")

    client.task_cancel("t")

    assert client.task_get_output("t") == b"Task canceled"


def test_failing_a_task_fails_the_tasks_waiting_on_it(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )
    client.task_add(
        "grandchild",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="child",
    )

    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"traceback", failed=True)

    # The failure runs the whole way down the graph,
    # and each task it reaches names the task whose run failed,
    # not the task next to it in the chain.
    for task_id in ["child", "grandchild"]:
        assert client.task_get_status(task_id) == TaskState.Failed
        assert client.task_get_output(task_id) == b"Dependency failed (task_id=parent)"

    # Nothing was released onto a queue by the failure.
    with pytest.raises(NoTaskAvailable):
        client.task_get(worker_id="w1", queue="work")


def test_canceling_a_parent_gives_its_children_the_canceled_output(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    client.task_cancel("parent")

    assert client.task_get_status("child") == TaskState.Canceled
    assert client.task_get_output("child") == b"Task canceled"


def test_a_child_of_a_failed_parent_is_added_failed(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"traceback", failed=True)

    # A failed parent never finishes,
    # so the child fails with it rather than waiting for it.
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    assert client.task_get_status("child") == TaskState.Failed
    assert client.task_get_output("child") == b"Dependency failed (task_id=parent)"


def test_one_failed_parent_fails_the_child(client):
    for task_id in ["a", "b"]:
        client.task_add(task_id, queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids=["a", "b"],
    )

    client.task_get(worker_id="w1", queue="work")
    client.task_done("a", worker_id="w1", output=b"", failed=False)
    assert client.task_get_status("child") == TaskState.Waiting

    # One parent that fails is enough, however the others ended.
    client.task_get(worker_id="w1", queue="work")
    client.task_done("b", worker_id="w1", output=b"traceback", failed=True)
    assert client.task_get_status("child") == TaskState.Failed


def test_a_failed_parent_decides_over_a_canceled_one(client):
    client.task_add("canceled", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add("failed", queue="work", priority=1.0, function=b"", input=b"")
    client.task_cancel("canceled")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("failed", worker_id="w1", output=b"traceback", failed=True)

    # The run that failed is the more useful of the two to report.
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids=["canceled", "failed"],
    )

    assert client.task_get_status("child") == TaskState.Failed
    assert client.task_get_output("child") == b"Dependency failed (task_id=failed)"


def test_cancel_a_failed_task_fails(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"traceback", failed=True)

    # Failed is final, so there is nothing to withdraw.
    assert client.task_cancel("t") is False
    assert client.task_get_status("t") == TaskState.Failed
    assert client.task_get_output("t") == b"traceback"


def test_get_worker_id_of_a_failed_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"traceback", failed=True)

    with pytest.raises(TaskStateError):
        client.task_get_worker_id("t")


def test_done_on_a_failed_task_raises(client):
    client.task_add("t", queue="work", priority=1.0, function=b"", input=b"")
    client.task_get(worker_id="w1", queue="work")
    client.task_done("t", worker_id="w1", output=b"traceback", failed=True)

    # Failed is final, the same way Finished is.
    with pytest.raises(TaskStateError):
        client.task_done("t", worker_id="w1", output=b"late", failed=False)


def test_count_by_state_counts_failed_tasks(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"traceback", failed=True)

    counts = client.task_get_count_by_state()
    assert (
        counts.waiting,
        counts.ready,
        counts.running,
        counts.finished,
        counts.failed,
        counts.canceled,
    ) == (0, 0, 0, 0, 2, 0)


def test_a_child_added_under_a_failed_chain_names_the_task_that_failed(client):
    client.task_add("parent", queue="work", priority=1.0, function=b"", input=b"")
    client.task_add(
        "child",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="parent",
    )

    client.task_get(worker_id="w1", queue="work")
    client.task_done("parent", worker_id="w1", output=b"traceback", failed=True)

    # child never ran, so it is not the task that failed.
    # A task added under it names the run that did.
    client.task_add(
        "grandchild",
        queue="work",
        priority=1.0,
        function=b"",
        input=b"",
        parent_task_ids="child",
    )

    assert client.task_get_status("grandchild") == TaskState.Failed
    assert client.task_get_output("grandchild") == b"Dependency failed (task_id=parent)"
