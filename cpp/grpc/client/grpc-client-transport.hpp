#pragma once

#include <memory>
#include <string>

#include "ds-service/client-transport.hpp"

namespace ds::grpc_client {

// A transport that calls the DsService of cpp/grpc/ds-service.proto at address.
// address is host:port, as gRPC takes it.
// The channel connects lazily, so an unreachable address fails the first call, not this one.
std::unique_ptr<ClientTransport> make_transport(std::string address, ClientOptions options);

} // namespace ds::grpc_client
