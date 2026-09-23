#include "client/grpc-client-transport.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "channel-settings.hpp"
#include "codec.hpp"
#include "ds-service/error.hpp"

namespace ds::grpc_client {

namespace {

// How often a waiting call polls ClientOptions::should_cancel.
// The comment on should_cancel in client-transport.hpp promises callers this interval,
// so change the two together.
constexpr auto CANCEL_POLL_INTERVAL = std::chrono::milliseconds(100);

// A timeout at or above this many seconds sets no deadline at all,
// because the deadline would overflow the clock.
constexpr double NO_DEADLINE_S = 1e9;

// Starts one call on the callback API of the stub.
// The generated method is overloaded, so it cannot be named as a member pointer.
#define DS_ASYNC(method)                                                       \
    [](auto* async, auto* context, auto* request, auto* response, auto done) { \
        async->method(context, request, response, std::move(done));            \
    }

// One call in flight: its context, and what its completion callback reports.
// The callback and the waiting thread share it,
// so it lives until both are done with it.
struct Call {
    grpc::ClientContext context;
    std::mutex lock;
    std::condition_variable completed;
    bool done = false;
    grpc::Status status;
};

class GrpcClientTransport final : public ClientTransport {
  public:
    GrpcClientTransport(std::string address, ClientOptions options)
        : options_(std::move(options)) {
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, grpc_settings::CLIENT_KEEPALIVE_TIME_MS);
        args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, grpc_settings::CLIENT_KEEPALIVE_TIMEOUT_MS);
        // 0 means "unlimited".
        // The cap is a total of pings sent while no call is in flight, not a rate,
        // so any finite value stops the pings on a long-idle connection,
        // the one connection keepalive_permit_without_calls protects.
        args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        args.SetMaxReceiveMessageSize(grpc_settings::MAX_MESSAGE_SIZE_BYTES);
        args.SetMaxSendMessageSize(grpc_settings::MAX_MESSAGE_SIZE_BYTES);

        channel_ = grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
        stub_ = ::DsService::NewStub(channel_);
    }

    void map_set(MapSetRequest request) override {
        unary<::MapSetRequest, ::Empty>(std::move(request), DS_ASYNC(MapSet));
    }

    MapGetResponse map_get(MapGetRequest request) override {
        return grpc_codec::decode(unary<::MapGetRequest, ::MapGetResponse>(std::move(request), DS_ASYNC(MapGet)));
    }

    SearchKeyResponse map_search_key(SearchKeyRequest request) override {
        return grpc_codec::decode(
            unary<::SearchKeyRequest, ::SearchKeyResponse>(std::move(request), DS_ASYNC(MapSearchKey)));
    }

    void task_add(TaskAddRequest request) override {
        unary<::TaskAddRequest, ::Empty>(std::move(request), DS_ASYNC(TaskAdd));
    }

    TaskGetStatusResponse task_get_status(TaskGetStatusRequest request) override {
        return grpc_codec::decode(
            unary<::TaskGetStatusRequest, ::TaskGetStatusResponse>(std::move(request), DS_ASYNC(TaskGetStatus)));
    }

    TaskGetOutputResponse task_get_output(TaskGetOutputRequest request) override {
        return grpc_codec::decode(
            unary<::TaskGetOutputRequest, ::TaskGetOutputResponse>(std::move(request), DS_ASYNC(TaskGetOutput)));
    }

    TaskGetCountByStateResponse task_get_count_by_state() override {
        return grpc_codec::decode(invoke<::TaskGetCountByStateResponse>(::Empty{}, DS_ASYNC(TaskGetCountByState)));
    }

    TaskCancelResponse task_cancel(TaskCancelRequest request) override {
        return grpc_codec::decode(
            unary<::TaskCancelRequest, ::TaskCancelResponse>(std::move(request), DS_ASYNC(TaskCancel)));
    }

    TaskGetPriorityResponse task_get_priority(TaskGetPriorityRequest request) override {
        return grpc_codec::decode(
            unary<::TaskGetPriorityRequest, ::TaskGetPriorityResponse>(std::move(request), DS_ASYNC(TaskGetPriority)));
    }

    void task_set_priority(TaskSetPriorityRequest request) override {
        unary<::TaskSetPriorityRequest, ::Empty>(std::move(request), DS_ASYNC(TaskSetPriority));
    }

    TaskGetWorkerIdResponse task_get_worker_id(TaskGetWorkerIdRequest request) override {
        return grpc_codec::decode(
            unary<::TaskGetWorkerIdRequest, ::TaskGetWorkerIdResponse>(std::move(request), DS_ASYNC(TaskGetWorkerId)));
    }

    SearchKeyResponse task_search_id(SearchKeyRequest request) override {
        return grpc_codec::decode(
            unary<::SearchKeyRequest, ::SearchKeyResponse>(std::move(request), DS_ASYNC(TaskSearchId)));
    }

    TaskGetResponse task_get(TaskGetRequest request) override {
        return grpc_codec::decode(unary<::TaskGetRequest, ::TaskGetResponse>(std::move(request), DS_ASYNC(TaskGet)));
    }

    void task_done(TaskDoneRequest request) override {
        unary<::TaskDoneRequest, ::Empty>(std::move(request), DS_ASYNC(TaskDone));
    }

    JournalSizeResponse journal_size(JournalSizeRequest request) override {
        return grpc_codec::decode(
            unary<::JournalSizeRequest, ::JournalSizeResponse>(std::move(request), DS_ASYNC(JournalSize)));
    }

    JournalReadResponse journal_read(JournalReadRequest request) override {
        return grpc_codec::decode(
            unary<::JournalReadRequest, ::JournalReadResponse>(std::move(request), DS_ASYNC(JournalRead)));
    }

    void journal_append(JournalAppendRequest request) override {
        unary<::JournalAppendRequest, ::Empty>(std::move(request), DS_ASYNC(JournalAppend));
    }

    SearchKeyResponse journal_search_key(SearchKeyRequest request) override {
        return grpc_codec::decode(
            unary<::SearchKeyRequest, ::SearchKeyResponse>(std::move(request), DS_ASYNC(JournalSearchKey)));
    }

    void time_series_append(TimeSeriesAppendRequest request) override {
        unary<::TimeSeriesAppendRequest, ::Empty>(std::move(request), DS_ASYNC(TimeSeriesAppend));
    }

    TimeSeriesGetResponse time_series_get(TimeSeriesGetRequest request) override {
        return grpc_codec::decode(
            unary<::TimeSeriesGetRequest, ::TimeSeriesGetResponse>(std::move(request), DS_ASYNC(TimeSeriesGet)));
    }

    SearchKeyResponse time_series_search_key(SearchKeyRequest request) override {
        return grpc_codec::decode(
            unary<::SearchKeyRequest, ::SearchKeyResponse>(std::move(request), DS_ASYNC(TimeSeriesSearchKey)));
    }

    MutexTryAcquireResponse mutex_try_acquire(MutexTryAcquireRequest request) override {
        return grpc_codec::decode(
            unary<::MutexTryAcquireRequest, ::MutexTryAcquireResponse>(std::move(request), DS_ASYNC(MutexTryAcquire)));
    }

    void mutex_release(MutexReleaseRequest request) override {
        unary<::MutexReleaseRequest, ::Empty>(std::move(request), DS_ASYNC(MutexRelease));
    }

    MutexGetWorkerIdResponse mutex_get_worker_id(MutexGetWorkerIdRequest request) override {
        return grpc_codec::decode(unary<::MutexGetWorkerIdRequest, ::MutexGetWorkerIdResponse>(
            std::move(request), DS_ASYNC(MutexGetWorkerId)));
    }

    SearchKeyResponse mutex_search_key(SearchKeyRequest request) override {
        return grpc_codec::decode(
            unary<::SearchKeyRequest, ::SearchKeyResponse>(std::move(request), DS_ASYNC(MutexSearchKey)));
    }

    CounterGetNextValueResponse counter_get_next_value(CounterGetNextValueRequest request) override {
        return grpc_codec::decode(unary<::CounterGetNextValueRequest, ::CounterGetNextValueResponse>(
            std::move(request), DS_ASYNC(CounterGetNextValue)));
    }

    CounterGetCurrentValueResponse counter_get_current_value(CounterGetCurrentValueRequest request) override {
        return grpc_codec::decode(unary<::CounterGetCurrentValueRequest, ::CounterGetCurrentValueResponse>(
            std::move(request), DS_ASYNC(CounterGetCurrentValue)));
    }

    SearchKeyResponse counter_search_key(SearchKeyRequest request) override {
        return grpc_codec::decode(
            unary<::SearchKeyRequest, ::SearchKeyResponse>(std::move(request), DS_ASYNC(CounterSearchKey)));
    }

    void close() override {
        std::scoped_lock guard{calls_lock_};
        closed_ = true;
        for (auto* call : calls_) {
            call->context.TryCancel();
        }
    }

  private:
    // Encode a plain request, then invoke the call with it.
    template <typename ProtoRequest, typename ProtoResponse, typename Plain, typename Start>
    ProtoResponse unary(Plain request, Start start) {
        ProtoRequest message;
        grpc_codec::encode(std::move(request), &message);
        return invoke<ProtoResponse>(message, start);
    }

    // Make one call and wait for it, polling should_cancel.
    // Returns the response, or throws ClientError,
    // or rethrows what should_cancel threw.
    //
    // The call always runs to completion before this returns or throws,
    // because the call reads request and the callback writes into response,
    // and both live on this stack frame.
    template <typename ProtoResponse, typename ProtoRequest, typename Start>
    ProtoResponse invoke(const ProtoRequest& request, Start start) {
        auto call = begin_call();

        ProtoResponse response;
        start(stub_->async(), &call->context, &request, &response, [call](grpc::Status status) {
            std::scoped_lock guard{call->lock};
            call->status = std::move(status);
            call->done = true;
            call->completed.notify_all();
        });

        std::exception_ptr failure;
        try {
            wait(*call);
        } catch (...) {
            // should_cancel threw.
            // Stop the call, and wait it out before unwinding.
            failure = std::current_exception();
            call->context.TryCancel();
            std::unique_lock guard{call->lock};
            call->completed.wait(guard, [&] { return call->done; });
        }

        const bool closed = end_call(call.get());
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!call->status.ok()) {
            throw to_error(call->status, closed);
        }
        return response;
    }

    // Register a new call, with the deadline set, so close() can cancel it.
    // Throws ClientError(ErrorCode::Closed) once close() has run.
    std::shared_ptr<Call> begin_call() {
        auto call = std::make_shared<Call>();
        if (options_.timeout.count() < NO_DEADLINE_S) {
            call->context.set_deadline(
                std::chrono::system_clock::now() +
                std::chrono::duration_cast<std::chrono::system_clock::duration>(options_.timeout));
        }

        std::scoped_lock guard{calls_lock_};
        if (closed_) {
            throw ClientError(ErrorCode::Closed, "The client is closed.");
        }
        calls_.insert(call.get());
        return call;
    }

    // Unregister a finished call, and report whether the client was closed by now.
    bool end_call(Call* call) {
        std::scoped_lock guard{calls_lock_};
        calls_.erase(call);
        return closed_;
    }

    // Block until the call completes.
    // With should_cancel set, wake up every CANCEL_POLL_INTERVAL to ask it,
    // and cancel the call the first time it says so.
    // A cancelled call still completes, with the status CANCELLED.
    void wait(Call& call) {
        std::unique_lock guard{call.lock};
        if (!options_.should_cancel) {
            call.completed.wait(guard, [&] { return call.done; });
            return;
        }

        bool cancelled = false;
        while (!call.completed.wait_for(guard, CANCEL_POLL_INTERVAL, [&] { return call.done; })) {
            if (cancelled) {
                continue;
            }

            // should_cancel can block, such as on the Python GIL,
            // so it runs without the lock the callback needs.
            guard.unlock();
            const bool cancel = options_.should_cancel();
            guard.lock();

            if (cancel) {
                call.context.TryCancel();
                cancelled = true;
            }
        }
    }

    // The ClientError for a failed status.
    // A call that close() cancelled reports Closed rather than Cancelled.
    static ClientError to_error(const grpc::Status& status, bool closed) {
        if (closed && status.error_code() == grpc::StatusCode::CANCELLED) {
            return ClientError(ErrorCode::Closed, "The client is closed.");
        }
        return ClientError(grpc_codec::from_status_code(status.error_code()), status.error_message());
    }

    ClientOptions options_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<::DsService::Stub> stub_;

    // Guards closed_ and calls_.
    // calls_ does not own its entries.
    // invoke keeps each call alive until end_call has removed it.
    std::mutex calls_lock_;
    bool closed_ = false;
    std::unordered_set<Call*> calls_;
};

#undef DS_ASYNC

} // namespace

std::unique_ptr<ClientTransport> make_transport(std::string address, ClientOptions options) {
    return std::make_unique<GrpcClientTransport>(std::move(address), std::move(options));
}

} // namespace ds::grpc_client
