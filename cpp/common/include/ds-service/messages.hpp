#pragma once

// The operations of ds-service, as plain C++ types.
//
// The comment on each request struct states the contract of its operation,
// under the snake_case name that both clients use,
// and the comment on a response struct states what its fields report.
// An operation with nothing to return returns Result<void> or void
// instead of a response struct,
// and task_get_count_by_state takes no request.
// A payload field is a std::string that can hold arbitrary binary data.
//
// See "The plain types define the system" in docs/developer-notes.md
// for how these types relate to the server core and to each transport.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ds-service/task-state.hpp"

namespace ds {

// map_set stores value under key, replacing any value already there.
struct MapSetRequest {
    std::string key;
    std::string value;
};

// map_get refuses with NotFound a key that no map_set has stored.
struct MapGetRequest {
    std::string key;
};

struct MapGetResponse {
    std::string value;
};

// map_search_key, journal_search_key, time_series_search_key, mutex_search_key
// and counter_search_key search the keys of their own structure.
// task_search_id searches the task_id of every task, in any state.
// pattern is an RE2 regular expression,
// and a key matches when the pattern matches any part of it.
// An invalid pattern is refused with InvalidArgument.
struct SearchKeyRequest {
    std::string pattern;
};

// The matching keys, in no guaranteed order.
struct SearchKeyResponse {
    std::vector<std::string> key;
};

// parent_task_ids names the tasks this one depends on.
// A task with a parent that is not Finished starts Waiting,
// unless a parent is Canceled or Failed,
// and no queue dispatches it until every parent finishes.
// Every parent must already exist, so a DAG is submitted parents first.
// task_add refuses with NotFound, and adds nothing,
// a parent it does not know.
// A task added with a Canceled parent is added Canceled,
// and one added with a Failed parent is added Failed,
// because neither parent ever finishes.
// A task with both a Canceled and a Failed parent is added Failed.
//
// task_add refuses with AlreadyExists a task_id already added.
// A Ready task waits on every queue it names,
// until a task_get on any one of them claims it.
struct TaskAddRequest {
    std::string task_id;
    std::vector<std::string> parent_task_ids;
    std::vector<std::string> queue;
    double priority = 0.0;
    std::string function;
    std::string input;
};

// task_get_status reports the state of every requested task_id,
// in the same order as the request,
// and Undefined for a task_id that does not exist.
struct TaskGetStatusRequest {
    std::vector<std::string> task_id;
};

struct TaskGetStatusResponse {
    std::vector<TaskState> state;
};

// task_get_output returns the output stored for the task,
// which is empty until the task is Finished, Failed or Canceled.
// It refuses with NotFound a task_id that does not exist.
struct TaskGetOutputRequest {
    std::string task_id;
};

struct TaskGetOutputResponse {
    std::string output;
};

// What task_get_count_by_state returns:
// the counts of all tasks in the system by state.
// Every task is in exactly one of these states,
// so the six counts sum to the total number of tasks the server knows about.
struct TaskGetCountByStateResponse {
    std::uint64_t waiting = 0;
    std::uint64_t ready = 0;
    std::uint64_t running = 0;
    std::uint64_t finished = 0;
    std::uint64_t failed = 0;
    std::uint64_t canceled = 0;
};

// task_cancel on a Ready task stops every queue from dispatching it.
// Canceling a Running task takes it away from the worker holding it.
// Canceling a task cancels every task waiting on it, and so on down the graph,
// because a canceled task never finishes.
// Every task canceled this way reports "Task canceled" as its output.
// task_cancel refuses with NotFound a task_id that does not exist.
struct TaskCancelRequest {
    std::string task_id;
};

// success is true if the task was Waiting, Ready or Running
// and this call moved it to Canceled,
// false if it was already Finished, Failed or Canceled
// and this call left it alone.
struct TaskCancelResponse {
    bool success = false;
};

// task_get_priority refuses with NotFound a task_id that does not exist.
struct TaskGetPriorityRequest {
    std::string task_id;
};

struct TaskGetPriorityResponse {
    double priority = 0.0;
};

// task_set_priority on a Ready task moves it within every queue it waits on,
// and it goes behind the tasks with equal priority already waiting.
// It refuses with NotFound a task_id that does not exist.
struct TaskSetPriorityRequest {
    std::string task_id;
    double priority = 0.0;
};

// task_get_worker_id reports the worker that claimed a Running task
// through task_get.
// It refuses with NotFound a task_id that does not exist,
// and with FailedPrecondition a task that is not Running.
struct TaskGetWorkerIdRequest {
    std::string task_id;
};

struct TaskGetWorkerIdResponse {
    std::string worker_id;
};

// task_get claims one Ready task for worker_id and moves it to Running.
// It tries the queues in the order the request lists them,
// and takes the highest priority task of the first queue that holds one.
// Tasks of equal priority leave in the order they entered the queue.
// It refuses with NotFound when no listed queue holds a Ready task.
struct TaskGetRequest {
    std::string worker_id;
    std::vector<std::string> queue;
};

struct TaskGetResponse {
    std::string task_id;
    std::string function;
    std::string input;
};

// worker_id must be the one that claimed the task through task_get.
// task_done from any other worker is refused with FailedPrecondition,
// and so is task_done on a task that is neither Running nor Canceled.
// task_done on a Canceled task is accepted from any worker, but does nothing.
// The task stays Canceled and the output is discarded.
// task_done refuses with NotFound a task_id that does not exist.
//
// failed says how the task ended.
// A task reported with failed = false is Finished,
// and one reported with failed = true is Failed.
// output is stored either way, so a failing worker can report why.
// Failing a task fails every task waiting on it, and so on down the graph,
// because a failed task never finishes.
// Every task failed this way reports
// "Dependency failed (task_id=...)" as its output,
// naming the task whose run failed.
struct TaskDoneRequest {
    std::string task_id;
    std::string output;
    std::string worker_id;
    bool failed = false;
};

// journal_size reports 0 for a key that has no journal.
struct JournalSizeRequest {
    std::string key;
};

struct JournalSizeResponse {
    std::uint64_t size = 0;
};

// journal_read returns the entries from index start up to end, exclusive,
// in the order they were appended.
// A range past the end of the journal is cut short there,
// and a key that has no journal reads as empty.
struct JournalReadRequest {
    std::string key;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

struct JournalReadResponse {
    std::vector<std::string> entry;
};

// journal_append adds value at the end of the journal under key,
// and creates the journal if it does not exist.
struct JournalAppendRequest {
    std::string key;
    std::string value;
};

// datetime is an ISO 8601 UTC datetime string,
// for example "2024-01-02T03:04:05Z" or "...+00:00".
// time_series_append refuses with InvalidArgument a datetime that does not parse.
struct TimeSeriesAppendRequest {
    std::string key;
    double value = 0.0;
    std::string datetime;
    std::int64_t step = 0;
};

// datetime is a UTC datetime string ending in "Z".
// It has six fractional digits, unless it falls on a whole second.
struct TimeSeriesDataPoint {
    double value = 0.0;
    std::string datetime;
    std::int64_t step = 0;
};

// start_time and start_step are inclusive lower bounds.
// end_time and end_step are exclusive upper bounds.
// A bound left as std::nullopt imposes no bound.
// Time bounds are ISO 8601 UTC datetime strings.
// An empty time bound imposes no bound either,
// and time_series_get refuses with InvalidArgument one that does not parse.
// time_series_get returns the points within every bound,
// in the order they were appended.
// A key that has no series returns no points.
struct TimeSeriesGetRequest {
    std::string key;
    std::optional<std::string> start_time;
    std::optional<std::string> end_time;
    std::optional<std::int64_t> start_step;
    std::optional<std::int64_t> end_step;
};

struct TimeSeriesGetResponse {
    std::vector<TimeSeriesDataPoint> point;
};

// mutex_try_acquire creates a mutex that does not exist.
// The server records worker_id as the holder of a mutex this call acquires,
// and it is what mutex_release must present to release it again.
struct MutexTryAcquireRequest {
    std::string key;
    std::string worker_id;
};

// acquired is true if the mutex was acquired by this call,
// false if any worker already holds it, this one included.
// The lock is not reentrant.
struct MutexTryAcquireResponse {
    bool acquired = false;
};

// worker_id must be the one that acquired the mutex.
// mutex_release clears the recorded holder, leaving the mutex free.
// The server refuses every other case with FailedPrecondition:
// a mutex held by another worker, one that is already free,
// and one that does not exist.
struct MutexReleaseRequest {
    std::string key;
    std::string worker_id;
};

// mutex_get_worker_id reports the worker holding the mutex.
// It refuses with NotFound a key no mutex_try_acquire has ever named,
// and with FailedPrecondition a key that exists but is free.
struct MutexGetWorkerIdRequest {
    std::string key;
};

struct MutexGetWorkerIdResponse {
    std::string worker_id;
};

// counter_get_next_value creates the counter on the first call for a given key,
// and returns 1.
// Each later call returns the previous value plus one.
struct CounterGetNextValueRequest {
    std::string key;
};

struct CounterGetNextValueResponse {
    std::uint64_t value = 0;
};

// counter_get_current_value returns the counter's current value,
// or 0 if the counter does not exist,
// and neither changes nor creates the counter.
struct CounterGetCurrentValueRequest {
    std::string key;
};

struct CounterGetCurrentValueResponse {
    std::uint64_t value = 0;
};

} // namespace ds
