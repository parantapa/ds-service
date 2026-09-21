#include <mutex>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service.hpp"

grpc::Status Map::set(const MapSetRequest* request, Empty*) {
    std::scoped_lock guard{lock};

    data[request->key()] = request->value();
    return grpc::Status::OK;
}

grpc::Status Map::get(const MapGetRequest* request, MapGetResponse* response) {
    std::scoped_lock guard{lock};

    auto it = data.find(request->key());
    if (it == data.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, fmt::format("Key {} not found.", request->key()));
    } else {
        response->set_value(it->second);
    }

    return grpc::Status::OK;
}

grpc::Status Map::search_key(const SearchKeyRequest* request, SearchKeyResponse* response) {
    RE2 pattern{request->pattern()};
    if (!pattern.ok()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            fmt::format("Invalid regular expression: {}", pattern.error()));
    }

    std::scoped_lock guard{lock};

    for (const auto& [key, _] : data) {
        if (RE2::PartialMatch(key, pattern)) {
            response->add_key(key);
        }
    }

    return grpc::Status::OK;
}
