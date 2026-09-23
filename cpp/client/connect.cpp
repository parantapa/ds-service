#include "ds-service/connect.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "client/grpc-client-transport.hpp"
#include "ds-service/error.hpp"

namespace ds {

std::unique_ptr<Client> connect(std::string_view address, ClientOptions options) {
    constexpr std::string_view SCHEME_SEPARATOR = "://";
    constexpr std::string_view GRPC_SCHEME = "grpc";

    std::string_view scheme = GRPC_SCHEME;
    std::string_view target = address;
    if (const auto separator = address.find(SCHEME_SEPARATOR); separator != std::string_view::npos) {
        scheme = address.substr(0, separator);
        target = address.substr(separator + SCHEME_SEPARATOR.size());
    }

    if (target.empty()) {
        throw ClientError(ErrorCode::InvalidArgument, "The address is empty.");
    }

    if (scheme == GRPC_SCHEME) {
        return std::make_unique<Client>(grpc_client::make_transport(std::string{target}, std::move(options)));
    }

    throw ClientError(ErrorCode::InvalidArgument,
                      "Unknown transport '" + std::string{scheme} + "' in address '" + std::string{address} + "'.");
}

} // namespace ds
