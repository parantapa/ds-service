#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>

#include "core/data-structures.hpp"

ds::Result<ds::JournalSizeResponse> JournalMap::size(ds::JournalSizeRequest request) {
    std::scoped_lock guard{lock};

    auto it = data.find(request.key);
    if (it == data.end()) {
        return ds::JournalSizeResponse{0};
    }

    return ds::JournalSizeResponse{it->second.size()};
}

ds::Result<ds::JournalReadResponse> JournalMap::read(ds::JournalReadRequest request) {
    std::scoped_lock guard{lock};

    ds::JournalReadResponse response;
    auto it = data.find(request.key);
    if (it != data.end()) {
        const auto& journal = it->second;
        auto size = journal.size();
        auto start = std::min<std::uint64_t>(request.start, size);
        auto end = std::min<std::uint64_t>(request.end, size);
        response.entry.reserve(end > start ? end - start : 0);
        for (auto index = start; index < end; index++) {
            response.entry.push_back(journal[index]);
        }
    }

    return response;
}

ds::Result<void> JournalMap::append(ds::JournalAppendRequest request) {
    std::scoped_lock guard{lock};

    data[std::move(request.key)].push_back(std::move(request.value));
    return {};
}

ds::Result<ds::SearchKeyResponse> JournalMap::search_key(ds::SearchKeyRequest request) {
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
