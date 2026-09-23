#include "server/grpc-server-transport.hpp"

#include <memory>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "channel-settings.hpp"
#include "codec.hpp"
#include "core/system-state.hpp"

namespace {

// A core Result as a gRPC status, with the value encoded into the response.
template <typename Plain, typename Proto>
grpc::Status reply(ds::Result<Plain>&& result, Proto* response) {
    if (!result) {
        return ds::grpc_codec::to_status(result.error());
    }
    ds::grpc_codec::encode(std::move(*result), response);
    return grpc::Status::OK;
}

// A core Result with no value, for an RPC that returns Empty.
grpc::Status reply(ds::Result<void>&& result, Empty*) {
    if (!result) {
        return ds::grpc_codec::to_status(result.error());
    }
    return grpc::Status::OK;
}

} // namespace

namespace ds::grpc_server {

// Each method here does nothing but decode the request,
// hand it to the data structure that owns the state,
// and encode what comes back.
// The locking and the logic live on that structure.
class DsServiceImpl final : public ::DsService::Service {
  public:
    explicit DsServiceImpl(SystemState& state)
        : state_(state) {}

    grpc::Status MapSet(grpc::ServerContext*, const ::MapSetRequest* request, ::Empty* response) override {
        return reply(state_.map.set(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status MapGet(grpc::ServerContext*, const ::MapGetRequest* request, ::MapGetResponse* response) override {
        return reply(state_.map.get(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status MapSearchKey(grpc::ServerContext*, const ::SearchKeyRequest* request,
                              ::SearchKeyResponse* response) override {
        return reply(state_.map.search_key(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskAdd(grpc::ServerContext*, const ::TaskAddRequest* request, ::Empty* response) override {
        return reply(state_.task_manager.add(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskGetStatus(grpc::ServerContext*, const ::TaskGetStatusRequest* request,
                               ::TaskGetStatusResponse* response) override {
        return reply(state_.task_manager.get_status(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskGetOutput(grpc::ServerContext*, const ::TaskGetOutputRequest* request,
                               ::TaskGetOutputResponse* response) override {
        return reply(state_.task_manager.get_output(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskGetCountByState(grpc::ServerContext*, const ::Empty*,
                                     ::TaskGetCountByStateResponse* response) override {
        return reply(state_.task_manager.get_count_by_state(), response);
    }

    grpc::Status TaskCancel(grpc::ServerContext*, const ::TaskCancelRequest* request,
                            ::TaskCancelResponse* response) override {
        return reply(state_.task_manager.cancel(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskGetPriority(grpc::ServerContext*, const ::TaskGetPriorityRequest* request,
                                 ::TaskGetPriorityResponse* response) override {
        return reply(state_.task_manager.get_priority(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskSetPriority(grpc::ServerContext*, const ::TaskSetPriorityRequest* request,
                                 ::Empty* response) override {
        return reply(state_.task_manager.set_priority(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskGetWorkerId(grpc::ServerContext*, const ::TaskGetWorkerIdRequest* request,
                                 ::TaskGetWorkerIdResponse* response) override {
        return reply(state_.task_manager.get_worker_id(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskSearchId(grpc::ServerContext*, const ::SearchKeyRequest* request,
                              ::SearchKeyResponse* response) override {
        return reply(state_.task_manager.search_id(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskGet(grpc::ServerContext*, const ::TaskGetRequest* request, ::TaskGetResponse* response) override {
        return reply(state_.task_manager.get(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TaskDone(grpc::ServerContext*, const ::TaskDoneRequest* request, ::Empty* response) override {
        return reply(state_.task_manager.done(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status JournalSize(grpc::ServerContext*, const ::JournalSizeRequest* request,
                             ::JournalSizeResponse* response) override {
        return reply(state_.journal_map.size(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status JournalRead(grpc::ServerContext*, const ::JournalReadRequest* request,
                             ::JournalReadResponse* response) override {
        return reply(state_.journal_map.read(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status JournalAppend(grpc::ServerContext*, const ::JournalAppendRequest* request,
                               ::Empty* response) override {
        return reply(state_.journal_map.append(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status JournalSearchKey(grpc::ServerContext*, const ::SearchKeyRequest* request,
                                  ::SearchKeyResponse* response) override {
        return reply(state_.journal_map.search_key(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TimeSeriesAppend(grpc::ServerContext*, const ::TimeSeriesAppendRequest* request,
                                  ::Empty* response) override {
        return reply(state_.time_series.append(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TimeSeriesGet(grpc::ServerContext*, const ::TimeSeriesGetRequest* request,
                               ::TimeSeriesGetResponse* response) override {
        return reply(state_.time_series.get(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status TimeSeriesSearchKey(grpc::ServerContext*, const ::SearchKeyRequest* request,
                                     ::SearchKeyResponse* response) override {
        return reply(state_.time_series.search_key(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status MutexTryAcquire(grpc::ServerContext*, const ::MutexTryAcquireRequest* request,
                                 ::MutexTryAcquireResponse* response) override {
        return reply(state_.mutexes.try_acquire(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status MutexRelease(grpc::ServerContext*, const ::MutexReleaseRequest* request, ::Empty* response) override {
        return reply(state_.mutexes.release(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status MutexGetWorkerId(grpc::ServerContext*, const ::MutexGetWorkerIdRequest* request,
                                  ::MutexGetWorkerIdResponse* response) override {
        return reply(state_.mutexes.get_worker_id(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status MutexSearchKey(grpc::ServerContext*, const ::SearchKeyRequest* request,
                                ::SearchKeyResponse* response) override {
        return reply(state_.mutexes.search_key(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status CounterGetNextValue(grpc::ServerContext*, const ::CounterGetNextValueRequest* request,
                                     ::CounterGetNextValueResponse* response) override {
        return reply(state_.counters.get_next_value(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status CounterGetCurrentValue(grpc::ServerContext*, const ::CounterGetCurrentValueRequest* request,
                                        ::CounterGetCurrentValueResponse* response) override {
        return reply(state_.counters.get_current_value(ds::grpc_codec::decode(*request)), response);
    }

    grpc::Status CounterSearchKey(grpc::ServerContext*, const ::SearchKeyRequest* request,
                                  ::SearchKeyResponse* response) override {
        return reply(state_.counters.search_key(ds::grpc_codec::decode(*request)), response);
    }

  private:
    SystemState& state_;
};

} // namespace ds::grpc_server

GrpcServerTransport::GrpcServerTransport(std::string address)
    : address_(std::move(address)) {}

GrpcServerTransport::~GrpcServerTransport() = default;

bool GrpcServerTransport::start(SystemState& state) {
    service_ = std::make_unique<ds::grpc_server::DsServiceImpl>(state);

    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder{};
    builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());

    // These are only correct as a pair with the client's keepalive settings.
    // See "The channel settings live in one header"
    // in docs/developer-notes.md.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, ds::grpc_settings::SERVER_KEEPALIVE_TIME_MS);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, ds::grpc_settings::SERVER_KEEPALIVE_TIMEOUT_MS);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
                               ds::grpc_settings::SERVER_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS);

    // Refuse to share the port,
    // which gRPC otherwise allows and nothing reports.
    // See "The server refuses to share its port"
    // in docs/developer-notes.md.
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);

    builder.SetMaxReceiveMessageSize(ds::grpc_settings::MAX_MESSAGE_SIZE_BYTES);
    builder.SetMaxSendMessageSize(ds::grpc_settings::MAX_MESSAGE_SIZE_BYTES);

    server_ = builder.BuildAndStart();
    if (!server_) {
        // BuildAndStart returns null when the port cannot be bound.
        spdlog::error("Failed to bind {}; is another ds-service already running there?", address_);
        return false;
    }

    return true;
}

void GrpcServerTransport::wait() {
    server_->Wait();
}

void GrpcServerTransport::shutdown(std::chrono::system_clock::time_point deadline) {
    server_->Shutdown(deadline);
}
