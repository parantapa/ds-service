#include "ds-service/client.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ds {

namespace {

// Base sleep, and its +/- jitter, between mutex_acquire attempts.
// The jitter keeps workers that contend for one mutex from retrying in lockstep.
// python/ds_service_client/client.py uses the same values.
constexpr std::chrono::duration<double> MUTEX_ACQUIRE_SLEEP{0.5};
constexpr std::chrono::duration<double> MUTEX_ACQUIRE_JITTER{0.1};

std::chrono::duration<double> mutex_retry_jitter() {
    thread_local std::mt19937 generator{std::random_device{}()};
    std::uniform_real_distribution<double> distribution{-MUTEX_ACQUIRE_JITTER.count(), MUTEX_ACQUIRE_JITTER.count()};
    return std::chrono::duration<double>{distribution(generator)};
}

} // namespace

Client::Client(std::unique_ptr<ClientTransport> transport)
    : transport_(std::move(transport)) {}

void Client::map_set(std::string key, std::string value) {
    transport_->map_set({std::move(key), std::move(value)});
}

std::string Client::map_get(std::string key) {
    return std::move(transport_->map_get({std::move(key)}).value);
}

std::vector<std::string> Client::map_search_key(std::string pattern) {
    return std::move(transport_->map_search_key({std::move(pattern)}).key);
}

void Client::task_add(std::string task_id, std::vector<std::string> parent_task_ids, std::vector<std::string> queue,
                      double priority, std::string function, std::string input) {
    transport_->task_add({
        .task_id = std::move(task_id),
        .parent_task_ids = std::move(parent_task_ids),
        .queue = std::move(queue),
        .priority = priority,
        .function = std::move(function),
        .input = std::move(input),
    });
}

std::vector<TaskState> Client::task_get_status(std::vector<std::string> task_id) {
    return std::move(transport_->task_get_status({std::move(task_id)}).state);
}

std::string Client::task_get_output(std::string task_id) {
    return std::move(transport_->task_get_output({std::move(task_id)}).output);
}

TaskGetCountByStateResponse Client::task_get_count_by_state() {
    return transport_->task_get_count_by_state();
}

bool Client::task_cancel(std::string task_id) {
    return transport_->task_cancel({std::move(task_id)}).success;
}

double Client::task_get_priority(std::string task_id) {
    return transport_->task_get_priority({std::move(task_id)}).priority;
}

void Client::task_set_priority(std::string task_id, double priority) {
    transport_->task_set_priority({std::move(task_id), priority});
}

std::string Client::task_get_worker_id(std::string task_id) {
    return std::move(transport_->task_get_worker_id({std::move(task_id)}).worker_id);
}

std::vector<std::string> Client::task_search_id(std::string pattern) {
    return std::move(transport_->task_search_id({std::move(pattern)}).key);
}

TaskGetResponse Client::task_get(std::string worker_id, std::vector<std::string> queue) {
    return transport_->task_get({std::move(worker_id), std::move(queue)});
}

void Client::task_done(std::string task_id, std::string worker_id, std::string output, bool failed) {
    transport_->task_done({
        .task_id = std::move(task_id),
        .output = std::move(output),
        .worker_id = std::move(worker_id),
        .failed = failed,
    });
}

std::uint64_t Client::journal_size(std::string key) {
    return transport_->journal_size({std::move(key)}).size;
}

std::vector<std::string> Client::journal_read(std::string key, std::uint64_t start, std::uint64_t end) {
    return std::move(transport_->journal_read({std::move(key), start, end}).entry);
}

void Client::journal_append(std::string key, std::string value) {
    transport_->journal_append({std::move(key), std::move(value)});
}

std::vector<std::string> Client::journal_search_key(std::string pattern) {
    return std::move(transport_->journal_search_key({std::move(pattern)}).key);
}

void Client::time_series_append(std::string key, double value, std::string datetime, std::int64_t step) {
    transport_->time_series_append({std::move(key), value, std::move(datetime), step});
}

std::vector<TimeSeriesDataPoint> Client::time_series_get(std::string key, std::optional<std::string> start_time,
                                                         std::optional<std::string> end_time,
                                                         std::optional<std::int64_t> start_step,
                                                         std::optional<std::int64_t> end_step) {
    return std::move(transport_
                         ->time_series_get({
                             .key = std::move(key),
                             .start_time = std::move(start_time),
                             .end_time = std::move(end_time),
                             .start_step = start_step,
                             .end_step = end_step,
                         })
                         .point);
}

std::vector<std::string> Client::time_series_search_key(std::string pattern) {
    return std::move(transport_->time_series_search_key({std::move(pattern)}).key);
}

bool Client::mutex_try_acquire(std::string key, std::string worker_id) {
    return transport_->mutex_try_acquire({std::move(key), std::move(worker_id)}).acquired;
}

void Client::mutex_release(std::string key, std::string worker_id) {
    transport_->mutex_release({std::move(key), std::move(worker_id)});
}

std::string Client::mutex_get_worker_id(std::string key) {
    return std::move(transport_->mutex_get_worker_id({std::move(key)}).worker_id);
}

std::vector<std::string> Client::mutex_search_key(std::string pattern) {
    return std::move(transport_->mutex_search_key({std::move(pattern)}).key);
}

void Client::mutex_acquire(std::string key, std::string worker_id,
                           std::optional<std::chrono::duration<double>> timeout) {
    // The server offers no blocking acquire, so this loop polls mutex_try_acquire.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (timeout) {
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(*timeout);
    }

    while (true) {
        if (mutex_try_acquire(key, worker_id)) {
            return;
        }

        auto delay =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(MUTEX_ACQUIRE_SLEEP + mutex_retry_jitter());
        if (deadline) {
            // Shorten the last sleep so the wait does not overshoot the deadline.
            const auto remaining = *deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                throw ClientError(ErrorCode::DeadlineExceeded, "Timed out acquiring mutex '" + key + "'.");
            }
            delay = std::min(delay, remaining);
        }
        std::this_thread::sleep_for(delay);
    }
}

std::uint64_t Client::counter_get_next_value(std::string key) {
    return transport_->counter_get_next_value({std::move(key)}).value;
}

std::uint64_t Client::counter_get_current_value(std::string key) {
    return transport_->counter_get_current_value({std::move(key)}).value;
}

std::vector<std::string> Client::counter_search_key(std::string pattern) {
    return std::move(transport_->counter_search_key({std::move(pattern)}).key);
}

void Client::close() {
    transport_->close();
}

} // namespace ds
