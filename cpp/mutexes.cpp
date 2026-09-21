#include <mutex>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service.hpp"

grpc::Status Mutexes::try_acquire(const MutexTryAcquireRequest* request, MutexTryAcquireResponse* response) {
    std::scoped_lock guard{lock};

    MutexState& mutex = data[request->key()];
    if (mutex.held) {
        response->set_acquired(false);
    } else {
        mutex.held = true;
        mutex.worker_id = request->worker_id();
        response->set_acquired(true);
    }

    return grpc::Status::OK;
}

grpc::Status Mutexes::release(const MutexReleaseRequest* request, Empty*) {
    std::scoped_lock guard{lock};

    auto it = data.find(request->key());
    if (it == data.end() || !it->second.held) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            fmt::format("Mutex {} is not held.", request->key()));
    }

    if (it->second.worker_id != request->worker_id()) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            fmt::format("Mutex {} is held by worker {}, not {}.", request->key(), it->second.worker_id,
                                        request->worker_id()));
    }

    it->second = MutexState{};
    return grpc::Status::OK;
}

grpc::Status Mutexes::get_worker_id(const MutexGetWorkerIdRequest* request, MutexGetWorkerIdResponse* response) {
    std::scoped_lock guard{lock};

    auto it = data.find(request->key());
    if (it == data.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, fmt::format("Mutex {} not found.", request->key()));
    }

    if (!it->second.held) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            fmt::format("Mutex {} is not held.", request->key()));
    }

    response->set_worker_id(it->second.worker_id);
    return grpc::Status::OK;
}

grpc::Status Mutexes::search_key(const SearchKeyRequest* request, SearchKeyResponse* response) {
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
