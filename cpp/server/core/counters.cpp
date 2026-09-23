#include <cstdint>
#include <mutex>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>

#include "core/data-structures.hpp"

ds::Result<ds::CounterGetNextValueResponse> Counters::get_next_value(ds::CounterGetNextValueRequest request) {
    std::scoped_lock guard{lock};

    std::uint64_t& counter = data[request.key];
    return ds::CounterGetNextValueResponse{++counter};
}

ds::Result<ds::CounterGetCurrentValueResponse> Counters::get_current_value(ds::CounterGetCurrentValueRequest request) {
    std::scoped_lock guard{lock};

    // find, not operator[], so that reading a counter does not create it,
    // and search_key does not start listing it.
    auto it = data.find(request.key);
    return ds::CounterGetCurrentValueResponse{it == data.end() ? 0 : it->second};
}

ds::Result<ds::SearchKeyResponse> Counters::search_key(ds::SearchKeyRequest request) {
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
