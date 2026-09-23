#pragma once

#include <chrono>
#include <functional>

#include "ds-service/messages.hpp"

namespace ds {

// How a client makes its calls.
struct ClientOptions {
    // The deadline of every call, measured from its start.
    std::chrono::duration<double> timeout = std::chrono::minutes(5);

    // Polled about every 100 ms while a call waits, from the calling thread.
    // Returning true cancels the call,
    // which then throws ClientError(ErrorCode::Cancelled).
    // Left empty, a call waits until it completes or its deadline passes.
    std::function<bool()> should_cancel{};
};

// A way for a client to reach the server, such as gRPC.
//
// There is one method per operation that ds-service/messages.hpp defines.
// Each takes the plain request and returns the plain response,
// where its operation has them,
// and throws ClientError on failure.
// Every method is safe to call from several threads at once.
class ClientTransport {
  public:
    virtual ~ClientTransport() = default;

    virtual void map_set(MapSetRequest request) = 0;
    virtual MapGetResponse map_get(MapGetRequest request) = 0;
    virtual SearchKeyResponse map_search_key(SearchKeyRequest request) = 0;

    virtual void task_add(TaskAddRequest request) = 0;
    virtual TaskGetStatusResponse task_get_status(TaskGetStatusRequest request) = 0;
    virtual TaskGetOutputResponse task_get_output(TaskGetOutputRequest request) = 0;
    virtual TaskGetCountByStateResponse task_get_count_by_state() = 0;
    virtual TaskCancelResponse task_cancel(TaskCancelRequest request) = 0;
    virtual TaskGetPriorityResponse task_get_priority(TaskGetPriorityRequest request) = 0;
    virtual void task_set_priority(TaskSetPriorityRequest request) = 0;
    virtual TaskGetWorkerIdResponse task_get_worker_id(TaskGetWorkerIdRequest request) = 0;
    virtual SearchKeyResponse task_search_id(SearchKeyRequest request) = 0;
    virtual TaskGetResponse task_get(TaskGetRequest request) = 0;
    virtual void task_done(TaskDoneRequest request) = 0;

    virtual JournalSizeResponse journal_size(JournalSizeRequest request) = 0;
    virtual JournalReadResponse journal_read(JournalReadRequest request) = 0;
    virtual void journal_append(JournalAppendRequest request) = 0;
    virtual SearchKeyResponse journal_search_key(SearchKeyRequest request) = 0;

    virtual void time_series_append(TimeSeriesAppendRequest request) = 0;
    virtual TimeSeriesGetResponse time_series_get(TimeSeriesGetRequest request) = 0;
    virtual SearchKeyResponse time_series_search_key(SearchKeyRequest request) = 0;

    virtual MutexTryAcquireResponse mutex_try_acquire(MutexTryAcquireRequest request) = 0;
    virtual void mutex_release(MutexReleaseRequest request) = 0;
    virtual MutexGetWorkerIdResponse mutex_get_worker_id(MutexGetWorkerIdRequest request) = 0;
    virtual SearchKeyResponse mutex_search_key(SearchKeyRequest request) = 0;

    virtual CounterGetNextValueResponse counter_get_next_value(CounterGetNextValueRequest request) = 0;
    virtual CounterGetCurrentValueResponse counter_get_current_value(CounterGetCurrentValueRequest request) = 0;
    virtual SearchKeyResponse counter_search_key(SearchKeyRequest request) = 0;

    // Cancel every call in flight, and make every later call throw ClientError(ErrorCode::Closed).
    // A call that close() cancels throws ClientError(ErrorCode::Closed) as well.
    // Safe to call more than once, and from any thread.
    virtual void close() = 0;
};

} // namespace ds
