#include <chrono>
#include <cstddef>
#include <format>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service.hpp"

std::optional<std::chrono::system_clock::time_point> parse_iso8601_utc(const std::string& s) {
    for (const char* fmt : {
             "%Y-%m-%dT%H:%M:%S%Ez",
             "%Y-%m-%dT%H:%M:%S%z",
             "%Y-%m-%dT%H:%M:%SZ",
             "%Y-%m-%dT%H:%M:%S",
         }) {
        std::istringstream ss{s};
        std::chrono::system_clock::time_point tp{};
        if (ss >> std::chrono::parse(std::string{fmt}, tp)) {
            ss >> std::ws;
            if (ss.eof()) {
                return tp;
            }
        }
    }
    return std::nullopt;
}

std::string format_iso8601_utc(const std::chrono::system_clock::time_point& tp) {
    auto secs = std::chrono::floor<std::chrono::seconds>(tp);
    if (secs == tp) {
        return std::format("{:%Y-%m-%dT%H:%M:%S}Z", secs);
    }
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", std::chrono::floor<std::chrono::microseconds>(tp));
}

grpc::Status TimeSeriesMap::append(const TimeSeriesAppendRequest* request, Empty*) {
    auto tp = parse_iso8601_utc(request->datetime());
    if (!tp) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            fmt::format("Invalid ISO 8601 UTC datetime: {}", request->datetime()));
    }

    std::scoped_lock guard{lock};

    auto& series = data[request->key()];
    series.value.push_back(request->value());
    series.time.push_back(*tp);
    series.step.push_back(request->step());

    return grpc::Status::OK;
}

grpc::Status TimeSeriesMap::get(const TimeSeriesGetRequest* request, TimeSeriesGetResponse* response) {
    // An empty time string means "no bound".
    // A non-empty one that fails to parse is an error.
    std::optional<std::chrono::system_clock::time_point> start_time{}, end_time{};
    if (request->has_start_time() && !request->start_time().empty()) {
        start_time = parse_iso8601_utc(request->start_time());
        if (!start_time) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid ISO 8601 UTC start_time: {}", request->start_time()));
        }
    }
    if (request->has_end_time() && !request->end_time().empty()) {
        end_time = parse_iso8601_utc(request->end_time());
        if (!end_time) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid ISO 8601 UTC end_time: {}", request->end_time()));
        }
    }

    bool has_start_step = request->has_start_step();
    bool has_end_step = request->has_end_step();

    std::scoped_lock guard{lock};

    auto it = data.find(request->key());
    if (it == data.end()) {
        return grpc::Status::OK;
    }

    const auto& series = it->second;
    for (std::size_t index = 0; index < series.value.size(); index++) {
        if (start_time && series.time[index] < *start_time) {
            continue;
        }
        if (end_time && series.time[index] >= *end_time) {
            continue;
        }
        if (has_start_step && series.step[index] < request->start_step()) {
            continue;
        }
        if (has_end_step && series.step[index] >= request->end_step()) {
            continue;
        }

        auto* point = response->add_point();
        point->set_value(series.value[index]);
        point->set_datetime(format_iso8601_utc(series.time[index]));
        point->set_step(series.step[index]);
    }

    return grpc::Status::OK;
}

grpc::Status TimeSeriesMap::search_key(const SearchKeyRequest* request, SearchKeyResponse* response) {
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
