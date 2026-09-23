#pragma once

// The messages that the server and its clients exchange, as plain C++ types.
//
// There is one struct per message in cpp/grpc/ds-service.proto,
// with the same name and the same field names,
// and the comments there state what each field means.
// The proto's Empty has no struct here:
// an operation that returns it returns Result<void> or void instead.
// A bytes field is a std::string that can hold arbitrary binary data.
// cpp/grpc/codec.cpp converts between these types and the protobuf messages.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ds-service/task-state.hpp"

namespace ds {

struct MapSetRequest {
    std::string key;
    std::string value;
};

struct MapGetRequest {
    std::string key;
};

struct MapGetResponse {
    std::string value;
};

struct SearchKeyRequest {
    std::string pattern;
};

struct SearchKeyResponse {
    std::vector<std::string> key;
};

struct TaskAddRequest {
    std::string task_id;
    std::vector<std::string> parent_task_ids;
    std::vector<std::string> queue;
    double priority = 0.0;
    std::string function;
    std::string input;
};

struct TaskGetStatusRequest {
    std::vector<std::string> task_id;
};

struct TaskGetStatusResponse {
    std::vector<TaskState> state;
};

struct TaskGetOutputRequest {
    std::string task_id;
};

struct TaskGetOutputResponse {
    std::string output;
};

struct TaskGetCountByStateResponse {
    std::uint64_t waiting = 0;
    std::uint64_t ready = 0;
    std::uint64_t running = 0;
    std::uint64_t finished = 0;
    std::uint64_t failed = 0;
    std::uint64_t canceled = 0;
};

struct TaskCancelRequest {
    std::string task_id;
};

struct TaskCancelResponse {
    bool success = false;
};

struct TaskGetPriorityRequest {
    std::string task_id;
};

struct TaskGetPriorityResponse {
    double priority = 0.0;
};

struct TaskSetPriorityRequest {
    std::string task_id;
    double priority = 0.0;
};

struct TaskGetWorkerIdRequest {
    std::string task_id;
};

struct TaskGetWorkerIdResponse {
    std::string worker_id;
};

struct TaskGetRequest {
    std::string worker_id;
    std::vector<std::string> queue;
};

struct TaskGetResponse {
    std::string task_id;
    std::string function;
    std::string input;
};

struct TaskDoneRequest {
    std::string task_id;
    std::string output;
    std::string worker_id;
    bool failed = false;
};

struct JournalSizeRequest {
    std::string key;
};

struct JournalSizeResponse {
    std::uint64_t size = 0;
};

struct JournalReadRequest {
    std::string key;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

struct JournalReadResponse {
    std::vector<std::string> entry;
};

struct JournalAppendRequest {
    std::string key;
    std::string value;
};

struct TimeSeriesAppendRequest {
    std::string key;
    double value = 0.0;
    std::string datetime;
    std::int64_t step = 0;
};

struct TimeSeriesDataPoint {
    double value = 0.0;
    std::string datetime;
    std::int64_t step = 0;
};

// A bound left as std::nullopt imposes no restriction,
// where the proto leaves the field unset.
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

struct MutexTryAcquireRequest {
    std::string key;
    std::string worker_id;
};

struct MutexTryAcquireResponse {
    bool acquired = false;
};

struct MutexReleaseRequest {
    std::string key;
    std::string worker_id;
};

struct MutexGetWorkerIdRequest {
    std::string key;
};

struct MutexGetWorkerIdResponse {
    std::string worker_id;
};

struct CounterGetNextValueRequest {
    std::string key;
};

struct CounterGetNextValueResponse {
    std::uint64_t value = 0;
};

struct CounterGetCurrentValueRequest {
    std::string key;
};

struct CounterGetCurrentValueResponse {
    std::uint64_t value = 0;
};

} // namespace ds
