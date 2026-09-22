#pragma once

// The types the server keeps its state in.
//
// Each struct below owns one top level data structure
// and the lock that guards it.
// A method named after an RPC serves that RPC:
// it takes the struct's own lock,
// and misc/ds-service.proto states the contract it answers with,
// down to the status code for each refusal.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include <parallel_hashmap/phmap.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

// The hash map that every top level data structure is built on.
template <typename K, typename V>
using HashMap = phmap::parallel_flat_hash_map<K, V>;

// The string to string map, with the lock that guards it.
struct Map {
    std::mutex lock{};

    HashMap<std::string, std::string> data{};

    grpc::Status set(const MapSetRequest* request, Empty* response);
    grpc::Status get(const MapGetRequest* request, MapGetResponse* response);
    grpc::Status search_key(const SearchKeyRequest* request, SearchKeyResponse* response);
};

// The map from a key to an append only journal,
// with the lock that guards it.
struct JournalMap {
    std::mutex lock{};

    HashMap<std::string, std::vector<std::string>> data{};

    grpc::Status size(const JournalSizeRequest* request, JournalSizeResponse* response);
    grpc::Status read(const JournalReadRequest* request, JournalReadResponse* response);
    grpc::Status append(const JournalAppendRequest* request, Empty* response);
    grpc::Status search_key(const SearchKeyRequest* request, SearchKeyResponse* response);
};

// Parse an ISO 8601 UTC datetime string into a system_clock time_point.
// Accepts a '+HH:MM'/'+HHMM' offset (converted to UTC),
// a trailing 'Z', or no designator (interpreted as UTC).
// Returns nullopt if the string does not parse.
std::optional<std::chrono::system_clock::time_point> parse_iso8601_utc(const std::string& s);

// Format a system_clock time_point as an ISO 8601 UTC datetime string.
// This function prints whole seconds without a fractional part.
// Otherwise it prints microseconds.
std::string format_iso8601_utc(const std::chrono::system_clock::time_point& tp);

// The points recorded under one key.
struct TimeSeries {
    std::vector<double> value;
    std::vector<std::chrono::system_clock::time_point> time;
    std::vector<std::int64_t> step;
};

// The map from a key to a time series,
// with the lock that guards it.
struct TimeSeriesMap {
    std::mutex lock{};

    HashMap<std::string, TimeSeries> data{};

    grpc::Status append(const TimeSeriesAppendRequest* request, Empty* response);
    grpc::Status get(const TimeSeriesGetRequest* request, TimeSeriesGetResponse* response);
    grpc::Status search_key(const SearchKeyRequest* request, SearchKeyResponse* response);
};

// One named mutex, as seen by the workers.
struct MutexState {
    bool held = false;
    std::string worker_id;
};

// The map from a key to a named mutex,
// with the lock that guards it.
struct Mutexes {
    std::mutex lock{};

    HashMap<std::string, MutexState> data{};

    grpc::Status try_acquire(const MutexTryAcquireRequest* request, MutexTryAcquireResponse* response);
    grpc::Status release(const MutexReleaseRequest* request, Empty* response);
    grpc::Status get_worker_id(const MutexGetWorkerIdRequest* request, MutexGetWorkerIdResponse* response);
    grpc::Status search_key(const SearchKeyRequest* request, SearchKeyResponse* response);
};

// The map from a key to a monotonic counter,
// with the lock that guards it.
struct Counters {
    std::mutex lock{};

    HashMap<std::string, std::uint64_t> data{};

    grpc::Status get_next_value(const CounterGetNextValueRequest* request, CounterGetNextValueResponse* response);
    grpc::Status get_current_value(const CounterGetCurrentValueRequest* request,
                                   CounterGetCurrentValueResponse* response);
    grpc::Status search_key(const SearchKeyRequest* request, SearchKeyResponse* response);
};

// One row's place in one queue.
// A row has an entry per queue it waits on,
// and a further entry for every seq it has held since.
struct TaskQueueEntry {
    double priority;
    std::uint64_t seq;
    std::size_t index;
};

// Orders a queue by priority, and by arrival among equal priorities.
struct TaskQueueEntryOrder {
    bool operator()(const TaskQueueEntry& a, const TaskQueueEntry& b) const;
};

using TaskQueue = std::priority_queue<TaskQueueEntry, std::vector<TaskQueueEntry>, TaskQueueEntryOrder>;

// The task rows, one vector per column.
struct TaskTable {
    std::vector<std::string> task_id;
    std::vector<std::string> function;
    std::vector<std::string> input;
    std::vector<std::string> output;
    std::vector<TaskState> state;
    std::vector<std::string> worker_id;
    std::vector<double> priority;
    std::vector<std::vector<std::string>> queues;
    std::vector<std::uint64_t> seq;

    // The dependency graph, one entry per row:
    // how many of the row's parents have yet to finish,
    // and the rows waiting on this one.
    std::vector<std::size_t> pending_parents;
    std::vector<std::vector<std::size_t>> children;

    // The row whose ending decided this row's.
    // A row that ended on its own account is its own origin,
    // and a row that inherited its ending from a parent
    // carries the origin that parent carried.
    // So every row in a chain names the one task that actually failed,
    // rather than the task next to it in the chain.
    std::vector<std::size_t> terminal_origin;
};

// The task table, its index and its queues,
// with the lock that guards them.
struct TaskManager {
    std::mutex lock{};

    TaskTable tasks{};

    HashMap<std::string, std::size_t> task_index{};

    std::uint64_t next_seq = 0;

    // Queue name -> the rows waiting on it, ordered by priority.
    // std::priority_queue is a max-heap,
    // so TaskGet dispatches the highest priority row first.
    HashMap<std::string, TaskQueue> queue{};

    grpc::Status add(const TaskAddRequest* request, Empty* response);
    grpc::Status get_status(const TaskGetStatusRequest* request, TaskGetStatusResponse* response);
    grpc::Status get_output(const TaskGetOutputRequest* request, TaskGetOutputResponse* response);
    grpc::Status get_count_by_state(const Empty* request, TaskGetCountByStateResponse* response);
    grpc::Status cancel(const TaskCancelRequest* request, TaskCancelResponse* response);
    grpc::Status get_priority(const TaskGetPriorityRequest* request, TaskGetPriorityResponse* response);
    grpc::Status set_priority(const TaskSetPriorityRequest* request, Empty* response);
    grpc::Status get_worker_id(const TaskGetWorkerIdRequest* request, TaskGetWorkerIdResponse* response);
    grpc::Status search_id(const SearchKeyRequest* request, SearchKeyResponse* response);
    grpc::Status get(const TaskGetRequest* request, TaskGetResponse* response);
    grpc::Status done(const TaskDoneRequest* request, Empty* response);

    // Push a row into each of its queues under a fresh seq.
    // The row must be Ready, and the caller must hold the lock.
    void enqueue(std::size_t index);

    // Move every row waiting on this one to a terminal state,
    // and with them every row waiting on those,
    // however deep the graph runs.
    // origin is the row whose own ending is being passed down,
    // which every row reached this way then carries and reports.
    // The row named here is left to the caller.
    // The caller must hold the lock.
    void propagate_to_children(std::size_t index, TaskState state, std::size_t origin);

    // What a row that never ran reports as its output.
    // A row that did run keeps whatever its worker reported.
    // The caller must hold the lock.
    std::string terminal_output(TaskState state, std::size_t origin) const;
};
