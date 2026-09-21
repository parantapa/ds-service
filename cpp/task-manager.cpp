#include <cstdint>
#include <mutex>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service.hpp"

bool TaskQueueEntryOrder::operator()(const TaskQueueEntry& a, const TaskQueueEntry& b) const {
    if (a.priority != b.priority) {
        return a.priority < b.priority;
    }
    return a.seq > b.seq;
}

grpc::Status TaskManager::add(const TaskAddRequest* request, Empty*) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request->task_id());
    if (it == task_index.end()) {
        tasks.task_id.push_back(request->task_id());
        tasks.function.push_back(request->function());
        tasks.input.push_back(request->input());
        tasks.output.push_back("");
        tasks.state.push_back(TaskState::Ready);
        tasks.worker_id.push_back("");
        tasks.priority.push_back(request->priority());
        tasks.queues.push_back({});
        tasks.seq.push_back(++next_seq);

        auto index = tasks.task_id.size() - 1;

        task_index[request->task_id()] = index;
        for (const auto& qname : request->queue()) {
            tasks.queues[index].push_back(qname);
            queue[qname].push(TaskQueueEntry{tasks.priority[index], tasks.seq[index], index});
        }

        return grpc::Status::OK;
    } else {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                            fmt::format("Task with ID = {} already exists.", request->task_id()));
    }
}

grpc::Status TaskManager::get_status(const TaskGetStatusRequest* request, TaskGetStatusResponse* response) {
    std::scoped_lock guard{lock};

    for (const auto& task_id : request->task_id()) {
        auto it = task_index.find(task_id);
        // TaskGetStatus reports Undefined for an unknown task_id.
        // That is not an error.
        if (it == task_index.end()) {
            response->add_state(TaskState::Undefined);
        } else {
            response->add_state(tasks.state[it->second]);
        }
    }

    return grpc::Status::OK;
}

grpc::Status TaskManager::get_output(const TaskGetOutputRequest* request, TaskGetOutputResponse* response) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request->task_id());
    if (it == task_index.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            fmt::format("Task with ID = {} not found.", request->task_id()));
    }

    response->set_output(tasks.output[it->second]);
    return grpc::Status::OK;
}

grpc::Status TaskManager::get_count_by_state(const Empty*, TaskGetCountByStateResponse* response) {
    std::scoped_lock guard{lock};

    std::uint64_t ready = 0, running = 0, complete = 0, canceled = 0;
    for (const auto& state : tasks.state) {
        switch (state) {
        case TaskState::Ready:
            ready++;
            break;
        case TaskState::Running:
            running++;
            break;
        case TaskState::Complete:
            complete++;
            break;
        case TaskState::Canceled:
            canceled++;
            break;
        default:
            break;
        }
    }

    response->set_ready(ready);
    response->set_running(running);
    response->set_complete(complete);
    response->set_canceled(canceled);
    return grpc::Status::OK;
}

grpc::Status TaskManager::cancel(const TaskCancelRequest* request, TaskCancelResponse* response) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request->task_id());
    if (it == task_index.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            fmt::format("Task with ID = {} not found.", request->task_id()));
    }

    auto index = it->second;

    if (tasks.state[index] != TaskState::Ready && tasks.state[index] != TaskState::Running) {
        response->set_success(false);
        return grpc::Status::OK;
    }

    tasks.state[index] = TaskState::Canceled;
    tasks.worker_id[index] = "";

    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status TaskManager::get_priority(const TaskGetPriorityRequest* request, TaskGetPriorityResponse* response) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request->task_id());
    if (it == task_index.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            fmt::format("Task with ID = {} not found.", request->task_id()));
    }

    response->set_priority(tasks.priority[it->second]);
    return grpc::Status::OK;
}

grpc::Status TaskManager::set_priority(const TaskSetPriorityRequest* request, Empty*) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request->task_id());
    if (it == task_index.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            fmt::format("Task with ID = {} not found.", request->task_id()));
    }

    auto index = it->second;
    tasks.priority[index] = request->priority();

    if (tasks.state[index] != TaskState::Ready) {
        return grpc::Status::OK;
    }

    tasks.seq[index] = ++next_seq;
    for (const auto& qname : tasks.queues[index]) {
        queue[qname].push(TaskQueueEntry{tasks.priority[index], tasks.seq[index], index});
    }

    return grpc::Status::OK;
}

grpc::Status TaskManager::get_worker_id(const TaskGetWorkerIdRequest* request, TaskGetWorkerIdResponse* response) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request->task_id());
    if (it == task_index.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            fmt::format("Task with ID = {} not found.", request->task_id()));
    }

    auto index = it->second;

    if (tasks.state[index] != TaskState::Running) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            fmt::format("Task with ID = {} is not Running.", request->task_id()));
    }

    response->set_worker_id(tasks.worker_id[index]);
    return grpc::Status::OK;
}

// Searches the task ids, which is the task queue's key space.
grpc::Status TaskManager::search_id(const SearchKeyRequest* request, SearchKeyResponse* response) {
    RE2 pattern{request->pattern()};
    if (!pattern.ok()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            fmt::format("Invalid regular expression: {}", pattern.error()));
    }

    std::scoped_lock guard{lock};

    for (const auto& task_id : tasks.task_id) {
        if (RE2::PartialMatch(task_id, pattern)) {
            response->add_key(task_id);
        }
    }

    return grpc::Status::OK;
}

grpc::Status TaskManager::get(const TaskGetRequest* request, TaskGetResponse* response) {
    std::scoped_lock guard{lock};

    // TaskGet searches the queues in the order the caller listed them:
    // the first one holding a Ready task wins.
    //
    // Dead queue entries are discarded lazily here,
    // as they reach the top of the heap.
    // A popped entry that is not usable is dropped rather than skipped.
    // See "Known limitations" in docs/developer-notes.md
    // for why they accumulate in the first place.
    for (const auto& qname : request->queue()) {
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
            //   either claimed through another of its queues or finished.
            // - TaskSetPriority pushed a newer entry that supersedes it.
            if (tasks.state[entry.index] != TaskState::Ready || entry.seq != tasks.seq[entry.index]) {
                continue;
            }

            const auto index = entry.index;
            tasks.state[index] = TaskState::Running;
            tasks.worker_id[index] = request->worker_id();

            response->set_task_id(tasks.task_id[index]);
            response->set_function(tasks.function[index]);
            response->set_input(tasks.input[index]);

            return grpc::Status::OK;
        }
    }

    return grpc::Status(grpc::StatusCode::NOT_FOUND, "No tasks available.");
}

grpc::Status TaskManager::done(const TaskDoneRequest* request, Empty*) {
    std::scoped_lock guard{lock};

    auto it = task_index.find(request->task_id());
    if (it == task_index.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            fmt::format("Task with ID = {} not found.", request->task_id()));
    }

    auto index = it->second;

    if (tasks.state[index] == TaskState::Canceled) {
        return grpc::Status::OK;
    }

    if (tasks.state[index] != TaskState::Running) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            fmt::format("Task with ID = {} is not Running.", request->task_id()));
    }

    if (tasks.worker_id[index] != request->worker_id()) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            fmt::format("Task with ID = {} is held by worker {}, not {}.", request->task_id(),
                                        tasks.worker_id[index], request->worker_id()));
    }

    tasks.state[index] = TaskState::Complete;
    tasks.output[index] = request->output();
    return grpc::Status::OK;
}
