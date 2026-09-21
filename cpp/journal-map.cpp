#include <algorithm>
#include <cstdint>
#include <mutex>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service.hpp"

grpc::Status JournalMap::size(const JournalSizeRequest* request, JournalSizeResponse* response) {
    std::scoped_lock guard{lock};

    auto it = data.find(request->key());
    if (it == data.end()) {
        response->set_size(0);
    } else {
        response->set_size(it->second.size());
    }

    return grpc::Status::OK;
}

grpc::Status JournalMap::read(const JournalReadRequest* request, JournalReadResponse* response) {
    std::scoped_lock guard{lock};

    auto it = data.find(request->key());
    if (it != data.end()) {
        const auto& journal = it->second;
        auto size = journal.size();
        auto start = std::min<std::uint64_t>(request->start(), size);
        auto end = std::min<std::uint64_t>(request->end(), size);
        for (auto index = start; index < end; index++) {
            response->add_entry(journal[index]);
        }
    }

    return grpc::Status::OK;
}

grpc::Status JournalMap::append(const JournalAppendRequest* request, Empty*) {
    std::scoped_lock guard{lock};

    data[request->key()].push_back(request->value());
    return grpc::Status::OK;
}

grpc::Status JournalMap::search_key(const SearchKeyRequest* request, SearchKeyResponse* response) {
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
