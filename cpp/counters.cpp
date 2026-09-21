#include <cstdint>
#include <mutex>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service.hpp"

grpc::Status Counters::get_next_value(const CounterGetNextValueRequest* request,
                                      CounterGetNextValueResponse* response) {
    std::scoped_lock guard{lock};

    std::uint64_t& counter = data[request->key()];
    response->set_value(++counter);

    return grpc::Status::OK;
}

grpc::Status Counters::get_current_value(const CounterGetCurrentValueRequest* request,
                                         CounterGetCurrentValueResponse* response) {
    std::scoped_lock guard{lock};

    auto it = data.find(request->key());
    response->set_value(it == data.end() ? 0 : it->second);

    return grpc::Status::OK;
}

grpc::Status Counters::search_key(const SearchKeyRequest* request, SearchKeyResponse* response) {
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
