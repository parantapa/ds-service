#include <mutex>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>

#include "core/data-structures.hpp"

ds::Result<ds::MutexTryAcquireResponse> Mutexes::try_acquire(ds::MutexTryAcquireRequest request) {
    std::scoped_lock guard{lock};

    MutexState& mutex = data[std::move(request.key)];
    if (mutex.held) {
        return ds::MutexTryAcquireResponse{false};
    }

    mutex.held = true;
    mutex.worker_id = std::move(request.worker_id);
    return ds::MutexTryAcquireResponse{true};
}

ds::Result<void> Mutexes::release(ds::MutexReleaseRequest request) {
    std::scoped_lock guard{lock};

    auto it = data.find(request.key);
    if (it == data.end() || !it->second.held) {
        return ds::make_error(ds::ErrorCode::FailedPrecondition, fmt::format("Mutex {} is not held.", request.key));
    }

    if (it->second.worker_id != request.worker_id) {
        return ds::make_error(ds::ErrorCode::FailedPrecondition,
                              fmt::format("Mutex {} is held by worker {}, not {}.", request.key, it->second.worker_id,
                                          request.worker_id));
    }

    // Reset rather than erase, so the key stays known:
    // search_key still lists it,
    // and get_worker_id reports it as not held rather than not found.
    it->second = MutexState{};
    return {};
}

ds::Result<ds::MutexGetWorkerIdResponse> Mutexes::get_worker_id(ds::MutexGetWorkerIdRequest request) {
    std::scoped_lock guard{lock};

    auto it = data.find(request.key);
    if (it == data.end()) {
        return ds::make_error(ds::ErrorCode::NotFound, fmt::format("Mutex {} not found.", request.key));
    }

    if (!it->second.held) {
        return ds::make_error(ds::ErrorCode::FailedPrecondition, fmt::format("Mutex {} is not held.", request.key));
    }

    return ds::MutexGetWorkerIdResponse{it->second.worker_id};
}

ds::Result<ds::SearchKeyResponse> Mutexes::search_key(ds::SearchKeyRequest request) {
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
