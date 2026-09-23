#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "transport.hpp"

namespace grpc {
class Server;
}

namespace ds::grpc_server {
class DsServiceImpl;
}

// Serves the DsService of cpp/grpc/ds-service.proto on one address.
class GrpcServerTransport final : public ServerTransport {
  public:
    // address is host:port, as gRPC takes it.
    explicit GrpcServerTransport(std::string address);
    ~GrpcServerTransport() override;

    std::string_view name() const override {
        return "grpc";
    }
    bool start(SystemState& state) override;
    void wait() override;
    void shutdown(std::chrono::system_clock::time_point deadline) override;

  private:
    std::string address_;
    std::unique_ptr<ds::grpc_server::DsServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
};
