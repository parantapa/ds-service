#include <mutex>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>

#include "core/data-structures.hpp"

ds::Result<void> Map::set(ds::MapSetRequest request) {
    std::scoped_lock guard{lock};

    data[std::move(request.key)] = std::move(request.value);
    return {};
}

ds::Result<ds::MapGetResponse> Map::get(ds::MapGetRequest request) {
    std::scoped_lock guard{lock};

    auto it = data.find(request.key);
    if (it == data.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Key {} not found.", request.key));
    }

    return ds::MapGetResponse{it->second};
}

ds::Result<ds::SearchKeyResponse> Map::search_key(ds::SearchKeyRequest request) {
    RE2 pattern{request.pattern};
    if (!pattern.ok()) {
        return ds::make_error(ds::ErrorCode::InvalidArgument,
                              fmt::format("Invalid regular expression: {}", pattern.error()));
    }

    std::scoped_lock guard{lock};

    ds::SearchKeyResponse response;
    for (const auto& [key, _] : data) {
        if (RE2::PartialMatch(key, pattern)) {
            response.key.push_back(key);
        }
    }

    return response;
}
