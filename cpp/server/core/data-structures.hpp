#pragma once

// The types the server keeps its state in.
//
// Each struct below owns one top level data structure
// and the lock that guards it.
// A method named after an RPC serves that RPC:
// it takes the struct's own lock,
// and cpp/grpc/ds-service.proto states the contract it answers with.
// Each method takes the plain request by value and moves what it keeps out of it.
// It returns the plain response, or the Error for a refusal.
// The method body holds the error code for each refusal,
// and the proto names only some of them.
//
// Nothing here depends on gRPC or protobuf.
// A transport decodes each request, calls the method, and encodes the result.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include <parallel_hashmap/phmap.h>

#include "ds-service/error.hpp"
#include "ds-service/messages.hpp"
#include "ds-service/task-state.hpp"

// The hash map that every top level data structure is built on.
template <typename K, typename V>
using HashMap = phmap::parallel_flat_hash_map<K, V>;

// The string to string map, with the lock that guards it.
struct Map {
    std::mutex lock{};

    HashMap<std::string, std::string> data{};

    ds::Result<void> set(ds::MapSetRequest request);
    ds::Result<ds::MapGetResponse> get(ds::MapGetRequest request);
    ds::Result<ds::SearchKeyResponse> search_key(ds::SearchKeyRequest request);
};

// The map from a key to an append only journal,
// with the lock that guards it.
struct JournalMap {
    std::mutex lock{};

    HashMap<std::string, std::vector<std::string>> data{};

    ds::Result<ds::JournalSizeResponse> size(ds::JournalSizeRequest request);
    ds::Result<ds::JournalReadResponse> read(ds::JournalReadRequest request);
    ds::Result<void> append(ds::JournalAppendRequest request);
    ds::Result<ds::SearchKeyResponse> search_key(ds::SearchKeyRequest request);
};

// Parse an ISO 8601 datetime string into a system_clock time_point, or return nullopt if it does not parse.
// The seconds can carry a fractional part.
// The string can end in a UTC offset such as '+HH:MM' or '+HHMM', which is converted to UTC,
// in a 'Z', or in no designator, which is read as UTC.
std::optional<std::chrono::system_clock::time_point> parse_iso8601_utc(const std::string& s);

// Format a system_clock time_point as an ISO 8601 UTC datetime string ending in 'Z'.
// A time_point on a whole second prints with no fractional part.
// Any other prints six fractional digits, truncated to the microsecond.
std::string format_iso8601_utc(const std::chrono::system_clock::time_point& tp);

// The points recorded under one key, one vector per column,
// in the order they were appended.
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

    ds::Result<void> append(ds::TimeSeriesAppendRequest request);
    ds::Result<ds::TimeSeriesGetResponse> get(ds::TimeSeriesGetRequest request);
    ds::Result<ds::SearchKeyResponse> search_key(ds::SearchKeyRequest request);
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

    ds::Result<ds::MutexTryAcquireResponse> try_acquire(ds::MutexTryAcquireRequest request);
    ds::Result<void> release(ds::MutexReleaseRequest request);
    ds::Result<ds::MutexGetWorkerIdResponse> get_worker_id(ds::MutexGetWorkerIdRequest request);
    ds::Result<ds::SearchKeyResponse> search_key(ds::SearchKeyRequest request);
};

// The map from a key to a monotonic counter,
// with the lock that guards it.
struct Counters {
    std::mutex lock{};

    HashMap<std::string, std::uint64_t> data{};

    ds::Result<ds::CounterGetNextValueResponse> get_next_value(ds::CounterGetNextValueRequest request);
    ds::Result<ds::CounterGetCurrentValueResponse> get_current_value(ds::CounterGetCurrentValueRequest request);
    ds::Result<ds::SearchKeyResponse> search_key(ds::SearchKeyRequest request);
};

// One row's place in one queue.
// A row has an entry per queue it waits on,
// and a further entry per queue for every seq it has held since.
struct TaskQueueEntry {
    double priority;
    std::uint64_t seq;
    std::size_t index;
};

// Orders a queue by priority, and by arrival among equal priorities.
struct TaskQueueEntryOrder {
    bool operator()(const TaskQueueEntry& a, const TaskQueueEntry& b) const;
};

// The entries of one queue, with the entry TaskGet tries first on top.
using TaskQueue = std::priority_queue<TaskQueueEntry, std::vector<TaskQueueEntry>, TaskQueueEntryOrder>;

// The task rows, one vector per column.
struct TaskTable {
    std::vector<std::string> task_id;
    std::vector<std::string> function;
    std::vector<std::string> input;
    std::vector<std::string> output;
    std::vector<ds::TaskState> state;
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
    // So every row in a chain names the one task that failed or was canceled,
    // rather than the task next to it in the chain.
    std::vector<std::size_t> terminal_origin;
};

// The task table, its index and its queues,
// with the lock that guards them.
struct TaskManager {
    std::mutex lock{};

    TaskTable tasks{};

    HashMap<std::string, std::size_t> task_index{};

    // The last seq handed out, not the next one: enqueue increments it first.
    // So no queue entry has seq 0, the seq of a row never enqueued.
    std::uint64_t next_seq = 0;

    // Queue name -> the rows waiting on it, ordered by priority.
    // std::priority_queue is a max-heap,
    // so TaskGet dispatches the highest priority row first.
    HashMap<std::string, TaskQueue> queue{};

    ds::Result<void> add(ds::TaskAddRequest request);
    ds::Result<ds::TaskGetStatusResponse> get_status(ds::TaskGetStatusRequest request);
    ds::Result<ds::TaskGetOutputResponse> get_output(ds::TaskGetOutputRequest request);
    ds::Result<ds::TaskGetCountByStateResponse> get_count_by_state();
    ds::Result<ds::TaskCancelResponse> cancel(ds::TaskCancelRequest request);
    ds::Result<ds::TaskGetPriorityResponse> get_priority(ds::TaskGetPriorityRequest request);
    ds::Result<void> set_priority(ds::TaskSetPriorityRequest request);
    ds::Result<ds::TaskGetWorkerIdResponse> get_worker_id(ds::TaskGetWorkerIdRequest request);
    // Serves TaskSearchId, over the task ids of rows in every state.
    ds::Result<ds::SearchKeyResponse> search_id(ds::SearchKeyRequest request);
    ds::Result<ds::TaskGetResponse> get(ds::TaskGetRequest request);
    ds::Result<void> done(ds::TaskDoneRequest request);

    // Push a row into each of its queues under a fresh seq.
    // The row must be Ready, and the caller must hold the lock.
    void enqueue(std::size_t index);

    // Move every row waiting on this one to state, which is Canceled or Failed,
    // and with them every row waiting on those,
    // however deep the graph runs.
    // origin is the row whose own ending is being passed down,
    // which every row reached this way then carries,
    // and which a row moved to Failed names in its output.
    // A row reached this way loses its worker and takes terminal_output as its output,
    // unless it is already Finished, Failed or Canceled, which leaves it alone.
    // The row named here is left to the caller.
    // The caller must hold the lock.
    void propagate_to_children(std::size_t index, ds::TaskState state, std::size_t origin);

    // What a Canceled row, or a row failed by a parent, reports as its output.
    // A row that its own worker finished or failed keeps whatever that worker reported.
    // The caller must hold the lock.
    std::string terminal_output(ds::TaskState state, std::size_t origin) const;
};
