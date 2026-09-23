#include <chrono>
#include <cstddef>
#include <format>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <spdlog/fmt/fmt.h>
#include <re2/re2.h>

#include "core/data-structures.hpp"

std::optional<std::chrono::system_clock::time_point> parse_iso8601_utc(const std::string& s) {
    for (const char* fmt : {
             "%Y-%m-%dT%H:%M:%S%Ez",
             "%Y-%m-%dT%H:%M:%S%z",
             "%Y-%m-%dT%H:%M:%SZ",
             "%Y-%m-%dT%H:%M:%S",
         }) {
        std::istringstream ss{s};
        std::chrono::system_clock::time_point tp{};
        // parse stops where the format ends,
        // so the eof test is what rejects anything left over but whitespace.
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

ds::Result<void> TimeSeriesMap::append(ds::TimeSeriesAppendRequest request) {
    auto tp = parse_iso8601_utc(request.datetime);
    if (!tp) {
        return ds::make_error(ds::ErrorCode::InvalidArgument,
                              fmt::format("Invalid ISO 8601 UTC datetime: {}", request.datetime));
    }

    std::scoped_lock guard{lock};

    auto& series = data[std::move(request.key)];
    series.value.push_back(request.value);
    series.time.push_back(*tp);
    series.step.push_back(request.step);

    return {};
}

ds::Result<ds::TimeSeriesGetResponse> TimeSeriesMap::get(ds::TimeSeriesGetRequest request) {
    std::optional<std::chrono::system_clock::time_point> start_time{}, end_time{};
    if (request.start_time && !request.start_time->empty()) {
        start_time = parse_iso8601_utc(*request.start_time);
        if (!start_time) {
            return ds::make_error(ds::ErrorCode::InvalidArgument,
                                  fmt::format("Invalid ISO 8601 UTC start_time: {}", *request.start_time));
        }
    }
    if (request.end_time && !request.end_time->empty()) {
        end_time = parse_iso8601_utc(*request.end_time);
        if (!end_time) {
            return ds::make_error(ds::ErrorCode::InvalidArgument,
                                  fmt::format("Invalid ISO 8601 UTC end_time: {}", *request.end_time));
        }
    }

    std::scoped_lock guard{lock};

    ds::TimeSeriesGetResponse response;
    auto it = data.find(request.key);
    if (it == data.end()) {
        return response;
    }

    const auto& series = it->second;
    for (std::size_t index = 0; index < series.value.size(); index++) {
        if (start_time && series.time[index] < *start_time) {
            continue;
        }
        if (end_time && series.time[index] >= *end_time) {
            continue;
        }
        if (request.start_step && series.step[index] < *request.start_step) {
            continue;
        }
        if (request.end_step && series.step[index] >= *request.end_step) {
            continue;
        }

        response.point.push_back({series.value[index], format_iso8601_utc(series.time[index]), series.step[index]});
    }

    return response;
}

ds::Result<ds::SearchKeyResponse> TimeSeriesMap::search_key(ds::SearchKeyRequest request) {
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
