#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>

#include "core/data-structures.hpp"

using ds::TaskState;

// What task_get_output reports for a Canceled task,
// and for a task that failed because a task it depends on failed.
// The failed one names the task whose run failed,
// which is the task a caller has to look at.
constexpr const char* CANCELED_OUTPUT = "Task canceled";
constexpr const char* DEPENDENCY_FAILED_FORMAT = "Dependency failed (task_id={})";

bool TaskQueueEntryOrder::operator()(const TaskQueueEntry& a, const TaskQueueEntry& b) const {
    if (a.priority != b.priority) {
        return a.priority < b.priority;
    }

    // Reversed on purpose.
    // The queue pops the entry that compares greatest,
    // so testing the other way round makes the lower seq the greater entry,
    // and rows of equal priority leave in the order they arrived.
    return a.seq > b.seq;
}

void TaskManager::enqueue(std::size_t index) {
    // The row takes a new seq every time it enters its queues,
    // so it waits behind the rows of equal priority already there,
    // and any entry left over from an earlier stay is now dead.
    tasks.seq[index] = ++next_seq;
    for (const auto& qname : tasks.queues[index]) {
        queue[qname].push(TaskQueueEntry{tasks.priority[index], tasks.seq[index], index});
    }
}

std::string TaskManager::terminal_output(TaskState state, std::size_t origin) const {
    if (state == TaskState::Failed) {
        return fmt::format(DEPENDENCY_FAILED_FORMAT, tasks.task_id[origin]);
    }
    return CANCELED_OUTPUT;
}

void TaskManager::propagate_to_children(std::size_t index, TaskState state, std::size_t origin) {
    const auto output = terminal_output(state, origin);

    // A row that is canceled or failed never finishes,
    // so every row waiting on it ends the same way.
    // The walk is iterative because the graph is as deep as the caller made it.
    // A row that is already final stops the walk there,
    // which is what terminates it when two paths meet on one row.
    std::vector<std::size_t> pending{tasks.children[index].begin(), tasks.children[index].end()};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();

        if (tasks.state[current] != TaskState::Waiting && tasks.state[current] != TaskState::Ready &&
            tasks.state[current] != TaskState::Running) {
            continue;
        }

        tasks.state[current] = state;
        tasks.worker_id[current] = "";
        tasks.output[current] = output;
        tasks.terminal_origin[current] = origin;

        for (const auto child : tasks.children[current]) {
            pending.push_back(child);
        }
    }
}

ds::Result<void> TaskManager::add(ds::TaskAddRequest request) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request.task_id);
    if (it != task_index.end()) {
        return ds::make_error(ds::ErrorCode::AlreadyExists,
                              fmt::format("Task with ID = {} already exists.", request.task_id));
    }

    // Every parent is resolved before the row is added,
    // so a request naming a parent the server does not know adds nothing.
    // See "The dependency graph is built at task_add"
    // in docs/developer-notes.md for what that rule buys.
    std::vector<std::size_t> parents;
    parents.reserve(request.parent_task_ids.size());
    for (const auto& parent_task_id : request.parent_task_ids) {
        auto parent_it = task_index.find(parent_task_id);
        if (parent_it == task_index.end()) {
            return ds::make_error(ds::ErrorCode::NotFound,
                                  fmt::format("Parent task with ID = {} not found.", parent_task_id));
        }
        parents.push_back(parent_it->second);
    }

    // A parent that has not finished holds the row back.
    // A canceled or failed parent never finishes,
    // so the row ends the same way rather than waiting forever.
    // A parent that failed decides this over one that was canceled,
    // because the run that failed is the more useful thing to report.
    std::size_t pending_parents = 0;
    std::optional<std::size_t> canceled_parent;
    std::optional<std::size_t> failed_parent;
    for (const auto parent : parents) {
        if (!canceled_parent && tasks.state[parent] == TaskState::Canceled) {
            canceled_parent = parent;
        }
        if (!failed_parent && tasks.state[parent] == TaskState::Failed) {
            failed_parent = parent;
        }
        if (tasks.state[parent] != TaskState::Finished) {
            pending_parents++;
        }
    }

    // The row takes the ending of the parent that decided it,
    // and the origin that parent carries with it,
    // so it names the task whose run failed
    // rather than the parent it was added under.
    // The origin is always a parent's here:
    // a row that has not run cannot be the origin of anything.
    const auto index = tasks.task_id.size();
    auto state = TaskState::Ready;
    auto origin = index;
    if (failed_parent) {
        state = TaskState::Failed;
        origin = tasks.terminal_origin[*failed_parent];
    } else if (canceled_parent) {
        state = TaskState::Canceled;
        origin = tasks.terminal_origin[*canceled_parent];
    } else if (pending_parents > 0) {
        state = TaskState::Waiting;
    }

    std::string output;
    if (state == TaskState::Failed || state == TaskState::Canceled) {
        output = terminal_output(state, origin);
    }

    task_index[request.task_id] = index;

    tasks.task_id.push_back(std::move(request.task_id));
    tasks.function.push_back(std::move(request.function));
    tasks.input.push_back(std::move(request.input));
    tasks.output.push_back(std::move(output));
    tasks.state.push_back(state);
    tasks.worker_id.push_back("");
    tasks.priority.push_back(request.priority);
    tasks.queues.push_back(std::move(request.queue));
    // A Ready row is enqueued below, which is what gives it its seq.
    tasks.seq.push_back(0);
    tasks.pending_parents.push_back(pending_parents);
    tasks.children.push_back({});
    tasks.terminal_origin.push_back(origin);

    // This list is how task_done and task_cancel reach the row.
    // The test must match the one that counted pending_parents above,
    // or the count and the releases drift apart.
    for (const auto parent : parents) {
        if (tasks.state[parent] != TaskState::Finished) {
            tasks.children[parent].push_back(index);
        }
    }

    if (state == TaskState::Ready) {
        enqueue(index);
    }

    return {};
}

ds::Result<ds::TaskGetStatusResponse> TaskManager::get_status(ds::TaskGetStatusRequest request) {
    std::scoped_lock guard{lock};

    ds::TaskGetStatusResponse response;
    response.state.reserve(request.task_id.size());
    for (const auto& task_id : request.task_id) {
        auto it = task_index.find(task_id);
        // task_get_status reports Undefined for an unknown task_id.
        // That is not an error.
        if (it == task_index.end()) {
            response.state.push_back(TaskState::Undefined);
        } else {
            response.state.push_back(tasks.state[it->second]);
        }
    }

    return response;
}

ds::Result<ds::TaskGetOutputResponse> TaskManager::get_output(ds::TaskGetOutputRequest request) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request.task_id);
    if (it == task_index.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Task with ID = {} not found.", request.task_id));
    }

    return ds::TaskGetOutputResponse{tasks.output[it->second]};
}

ds::Result<ds::TaskGetCountByStateResponse> TaskManager::get_count_by_state() {
    std::scoped_lock guard{lock};

    std::uint64_t waiting = 0, ready = 0, running = 0, finished = 0, failed = 0, canceled = 0;
    for (const auto& state : tasks.state) {
        switch (state) {
        case TaskState::Waiting:
            waiting++;
            break;
        case TaskState::Ready:
            ready++;
            break;
        case TaskState::Running:
            running++;
            break;
        case TaskState::Finished:
            finished++;
            break;
        case TaskState::Failed:
            failed++;
            break;
        case TaskState::Canceled:
            canceled++;
            break;
        default:
            break;
        }
    }

    return ds::TaskGetCountByStateResponse{
        .waiting = waiting,
        .ready = ready,
        .running = running,
        .finished = finished,
        .failed = failed,
        .canceled = canceled,
    };
}

ds::Result<ds::TaskCancelResponse> TaskManager::cancel(ds::TaskCancelRequest request) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request.task_id);
    if (it == task_index.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Task with ID = {} not found.", request.task_id));
    }

    auto index = it->second;

    if (tasks.state[index] != TaskState::Waiting && tasks.state[index] != TaskState::Ready &&
        tasks.state[index] != TaskState::Running) {
        return ds::TaskCancelResponse{false};
    }

    tasks.state[index] = TaskState::Canceled;
    tasks.worker_id[index] = "";
    tasks.terminal_origin[index] = index;
    tasks.output[index] = terminal_output(TaskState::Canceled, index);
    propagate_to_children(index, TaskState::Canceled, index);

    return ds::TaskCancelResponse{true};
}

ds::Result<ds::TaskGetPriorityResponse> TaskManager::get_priority(ds::TaskGetPriorityRequest request) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request.task_id);
    if (it == task_index.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Task with ID = {} not found.", request.task_id));
    }

    return ds::TaskGetPriorityResponse{tasks.priority[it->second]};
}

ds::Result<void> TaskManager::set_priority(ds::TaskSetPriorityRequest request) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request.task_id);
    if (it == task_index.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Task with ID = {} not found.", request.task_id));
    }

    auto index = it->second;
    tasks.priority[index] = request.priority;

    // A row that is not Ready is in no queue.
    // A Waiting row enters its queues at the new priority when enqueue runs for it.
    if (tasks.state[index] != TaskState::Ready) {
        return {};
    }

    // The entry at the old priority stays in each queue as a dead entry.
    // See "Known limitations" in docs/developer-notes.md.
    enqueue(index);

    return {};
}

ds::Result<ds::TaskGetWorkerIdResponse> TaskManager::get_worker_id(ds::TaskGetWorkerIdRequest request) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request.task_id);
    if (it == task_index.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Task with ID = {} not found.", request.task_id));
    }

    auto index = it->second;

    if (tasks.state[index] != TaskState::Running) {
        return ds::make_error(ds::ErrorCode::FailedPrecondition,
                              fmt::format("Task with ID = {} is not Running.", request.task_id));
    }

    return ds::TaskGetWorkerIdResponse{tasks.worker_id[index]};
}

ds::Result<ds::SearchKeyResponse> TaskManager::search_id(ds::SearchKeyRequest request) {
    RE2 pattern{request.pattern};
    if (!pattern.ok()) {
        return ds::make_error(ds::ErrorCode::InvalidArgument,
                              fmt::format("Invalid regular expression: {}", pattern.error()));
    }

    std::scoped_lock guard{lock};

    ds::SearchKeyResponse response;
    for (const auto& task_id : tasks.task_id) {
        if (RE2::PartialMatch(task_id, pattern)) {
            response.key.push_back(task_id);
        }
    }

    return response;
}

ds::Result<ds::TaskGetResponse> TaskManager::get(ds::TaskGetRequest request) {
    std::scoped_lock guard{lock};

    // Dead queue entries are discarded lazily here,
    // as they reach the top of the heap.
    // A popped entry that is not usable is dropped rather than skipped.
    // See "Known limitations" in docs/developer-notes.md
    // for why they accumulate in the first place.
    for (const auto& qname : request.queue) {
        auto queue_it = queue.find(qname);
        if (queue_it == queue.end()) {
            continue;
        }

        auto& task_queue = queue_it->second;
        while (!task_queue.empty()) {
            const auto entry = task_queue.top();
            task_queue.pop();

            // An entry can be dead in two ways:
            // - Its row left Ready,
            //   either claimed through another of its queues or canceled.
            // - task_set_priority pushed a newer entry that supersedes it.
            if (tasks.state[entry.index] != TaskState::Ready || entry.seq != tasks.seq[entry.index]) {
                continue;
            }

            const auto index = entry.index;
            tasks.state[index] = TaskState::Running;
            tasks.worker_id[index] = std::move(request.worker_id);

            return ds::TaskGetResponse{
                .task_id = tasks.task_id[index],
                .function = tasks.function[index],
                .input = tasks.input[index],
            };
        }
    }

    return ds::make_error(ds::ErrorCode::NotFound, "No tasks available.");
}

ds::Result<void> TaskManager::done(ds::TaskDoneRequest request) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request.task_id);
    if (it == task_index.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Task with ID = {} not found.", request.task_id));
    }

    auto index = it->second;

    // The worker may still hold a task canceled under it.
    // Its report is accepted and dropped, so the task stays Canceled.
    if (tasks.state[index] == TaskState::Canceled) {
        return {};
    }

    if (tasks.state[index] != TaskState::Running) {
        return ds::make_error(ds::ErrorCode::FailedPrecondition,
                              fmt::format("Task with ID = {} is not Running.", request.task_id));
    }

    if (tasks.worker_id[index] != request.worker_id) {
        return ds::make_error(ds::ErrorCode::FailedPrecondition,
                              fmt::format("Task with ID = {} is held by worker {}, not {}.", request.task_id,
                                          tasks.worker_id[index], request.worker_id));
    }

    tasks.output[index] = std::move(request.output);

    // A row that failed never finishes,
    // so the rows waiting on it fail with it rather than waiting forever.
    if (request.failed) {
        tasks.state[index] = TaskState::Failed;
        tasks.terminal_origin[index] = index;
        propagate_to_children(index, TaskState::Failed, index);
        return {};
    }

    tasks.state[index] = TaskState::Finished;

    // Finishing a row releases the rows that were waiting on it.
    // See "The dependency graph is built at task_add"
    // in docs/developer-notes.md for why the count lands on zero
    // exactly when the last parent finishes.
    for (const auto child : tasks.children[index]) {
        tasks.pending_parents[child]--;
        if (tasks.pending_parents[child] == 0 && tasks.state[child] == TaskState::Waiting) {
            tasks.state[child] = TaskState::Ready;
            enqueue(child);
        }
    }

    return {};
}
