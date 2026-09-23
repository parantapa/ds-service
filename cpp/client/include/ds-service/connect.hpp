#pragma once

#include <memory>
#include <string_view>

#include "ds-service/client.hpp"

namespace ds {

// Connect to the server at address, over the transport the address names.
//
// A bare host:port and grpc://host:port both select gRPC.
// The connection is made lazily,
// so an unreachable server fails the first call rather than this one.
// Throw ClientError(ErrorCode::InvalidArgument) for an empty address,
// or for a scheme that names no transport.
//
// This function lives in the ds-service-connect library,
// which links every transport.
// ds-service-client alone links none of them.
std::unique_ptr<Client> connect(std::string_view address, ClientOptions options = {});

} // namespace ds
