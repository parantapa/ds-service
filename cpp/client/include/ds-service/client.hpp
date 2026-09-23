#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ds-service/client-transport.hpp"
#include "ds-service/error.hpp"
#include "ds-service/messages.hpp"
#include "ds-service/task-state.hpp"

namespace ds {

// A connection to a ds-service server, and the operations it offers.
//
// Every method throws ClientError on failure,
// and the comments in ds-service/messages.hpp state what each operation does.
// The methods are safe to call from several threads at once.
// Destroying the client while another thread is in a call is not.
//
// ds::connect() in ds-service/connect.hpp returns one for an address.
class Client {
  public:
    explicit Client(std::unique_ptr<ClientTransport> transport);

    void map_set(std::string key, std::string value);
    std::string map_get(std::string key);
    std::vector<std::string> map_search_key(std::string pattern);

    void task_add(std::string task_id, std::vector<std::string> parent_task_ids, std::vector<std::string> queue,
                  double priority, std::string function, std::string input);
    // One state per id, in the same order. An unknown id reports TaskState::Undefined.
    std::vector<TaskState> task_get_status(std::vector<std::string> task_id);
    std::string task_get_output(std::string task_id);
    TaskGetCountByStateResponse task_get_count_by_state();
    // True if this call moved the task to Canceled.
    bool task_cancel(std::string task_id);
    double task_get_priority(std::string task_id);
    void task_set_priority(std::string task_id, double priority);
    std::string task_get_worker_id(std::string task_id);
    std::vector<std::string> task_search_id(std::string pattern);
    // The queues are searched in the order listed,
    // and the first one holding a Ready task supplies it.
    // Throws ClientError(ErrorCode::NotFound) when no queue has a task ready.
    TaskGetResponse task_get(std::string worker_id, std::vector<std::string> queue);
    void task_done(std::string task_id, std::string worker_id, std::string output, bool failed = false);

    std::uint64_t journal_size(std::string key);
    // The entries in the half-open index range [start, end), clamped to the journal.
    std::vector<std::string> journal_read(std::string key, std::uint64_t start, std::uint64_t end);
    void journal_append(std::string key, std::string value);
    std::vector<std::string> journal_search_key(std::string pattern);

    void time_series_append(std::string key, double value, std::string datetime, std::int64_t step = 0);
    // A bound left as std::nullopt imposes no restriction.
    std::vector<TimeSeriesDataPoint> time_series_get(std::string key,
                                                     std::optional<std::string> start_time = std::nullopt,
                                                     std::optional<std::string> end_time = std::nullopt,
                                                     std::optional<std::int64_t> start_step = std::nullopt,
                                                     std::optional<std::int64_t> end_step = std::nullopt);
    std::vector<std::string> time_series_search_key(std::string pattern);

    // True if this call acquired the mutex. The lock is not reentrant.
    bool mutex_try_acquire(std::string key, std::string worker_id);
    void mutex_release(std::string key, std::string worker_id);
    std::string mutex_get_worker_id(std::string key);
    std::vector<std::string> mutex_search_key(std::string pattern);

    // Retry mutex_try_acquire until it succeeds, sleeping about half a second between attempts.
    // With a timeout, throws ClientError(ErrorCode::DeadlineExceeded) once it has elapsed,
    // sleeps included. With std::nullopt, retries forever.
    // Each attempt is an ordinary call, which ClientOptions::should_cancel can cancel,
    // but the sleeps between them do not poll it.
    void mutex_acquire(std::string key, std::string worker_id,
                       std::optional<std::chrono::duration<double>> timeout = std::nullopt);

    std::uint64_t counter_get_next_value(std::string key);
    std::uint64_t counter_get_current_value(std::string key);
    std::vector<std::string> counter_search_key(std::string pattern);

    // Cancel every call in flight, and make every later call throw ClientError(ErrorCode::Closed).
    // Safe to call more than once.
    void close();

  private:
    std::unique_ptr<ClientTransport> transport_;
};

} // namespace ds
