#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <experimental/scope>

#include <spdlog/spdlog.h>
#include <argparse/argparse.hpp>
#include <parallel_hashmap/phmap.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>
#include <task_table.hpp>

template <typename K, typename V>
using Map = phmap::parallel_flat_hash_map<K, V>;

using TaskQueueEntry = std::priority_queue<std::pair<double, std::size_t>>;

struct TaskManager {
    task_table::TaskTable tasks;
    Map<std::string, std::size_t> task_index;
    Map<std::string, TaskQueueEntry> queue;
};

struct TimeSeries {
    std::vector<double> value;
    std::vector<std::chrono::system_clock::time_point> time;
    std::vector<std::int64_t> step;
};

struct SystemState {
    std::mutex lock{};
    Map<std::string, std::string> map{};
    Map<std::string, std::vector<std::string>> journal_map{};
    Map<std::string, TimeSeries> time_series{};
    TaskManager task_manager{};
    grpc::Server* server{nullptr};
    bool shutdown{false};
};

// Parse an ISO 8601 UTC datetime string into a system_clock time_point.
// Accepts a '+HH:MM'/'+HHMM' offset (converted to UTC),
// a trailing 'Z', or no designator (interpreted as UTC).
// Returns nullopt if the string does not parse.
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

// Format a system_clock time_point as an ISO 8601 UTC datetime string.
// Whole seconds are rendered without a fractional part;
// otherwise microseconds are used.
std::string format_iso8601_utc(const std::chrono::system_clock::time_point& tp) {
    auto secs = std::chrono::floor<std::chrono::seconds>(tp);
    if (secs == tp) {
        return std::format("{:%Y-%m-%dT%H:%M:%S}Z", secs);
    }
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", std::chrono::floor<std::chrono::microseconds>(tp));
}

// Current time in seconds as a double.
// Only differences between two readings are meaningful; the epoch is arbitrary.
double now_seconds() {
    using namespace std::chrono;
    return duration<double>(high_resolution_clock::now().time_since_epoch()).count();
}

SystemState* GLOBAL_SYSTEM_STATE = nullptr;

struct DsServiceImpl final : public DsService::Service {
    grpc::Status MapSet(grpc::ServerContext*, const MapSetRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        GLOBAL_SYSTEM_STATE->map[request->key()] = request->value();
        return grpc::Status::OK;
    }

    grpc::Status MapGet(grpc::ServerContext*, const MapGetRequest* request, MapGetResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& map = GLOBAL_SYSTEM_STATE->map;
        auto it = map.find(request->key());
        if (it == map.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, fmt::format("Key {} not found.", request->key()));
        } else {
            response->set_value(it->second);
        }

        return grpc::Status::OK;
    }

    grpc::Status MapSearchKey(grpc::ServerContext*, const MapSearchKeyRequest* request,
                              MapSearchKeyResponse* response) override {
        RE2 pattern{request->pattern()};
        if (!pattern.ok()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid regular expression: {}", pattern.error()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        for (const auto& [key, _] : GLOBAL_SYSTEM_STATE->map) {
            if (RE2::PartialMatch(key, pattern)) {
                response->add_key(key);
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status TaskAdd(grpc::ServerContext*, const TaskAddRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto it = task_manager.task_index.find(request->task_id());
        if (it == task_manager.task_index.end()) {
            task_manager.tasks.push_back(request->task_id(), request->priority(), request->function(), request->input(),
                                         "", TaskState::Ready, -1.0, {});

            auto index = task_manager.tasks.size() - 1;
            auto task = task_manager.tasks[index];

            task_manager.task_index[request->task_id()] = index;
            for (const auto& qname : request->queue()) {
                task.queues().push_back(qname);
                task_manager.queue[qname].push(std::make_pair(request->priority(), index));
            }

            return grpc::Status::OK;
        } else {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                                fmt::format("Task with ID = {} already exists.", request->task_id()));
        }
    }

    grpc::Status TaskStatus(grpc::ServerContext*, const TaskStatusRequest* request,
                            TaskStatusResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto it = task_manager.task_index.find(request->task_id());
        if (it == task_manager.task_index.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                fmt::format("Task with ID = {} not found.", request->task_id()));
        } else {
            auto index = task_manager.task_index[request->task_id()];
            const auto& task = task_manager.tasks[index];
            response->set_state(task.state());
            response->set_output(task.output());
            return grpc::Status::OK;
        }
    }

    grpc::Status TaskGet(grpc::ServerContext*, const TaskGetRequest* request, TaskGetResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        for (const auto& qname : request->queue()) {
            auto& queue = task_manager.queue[qname];
            while (!queue.empty()) {
                const auto& [_, index] = queue.top();
                auto task = task_manager.tasks[index];
                if (task.state() == TaskState::Ready) {
                    task.state() = TaskState::Running;
                    task.start_time() = now_seconds();

                    response->set_task_id(task.task_id());
                    response->set_function(task.function());
                    response->set_input(task.input());

                    queue.pop();
                    return grpc::Status::OK;
                } else {
                    queue.pop();
                }
            }
        }

        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No tasks available.");
    }

    grpc::Status TaskDone(grpc::ServerContext*, const TaskDoneRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto it = task_manager.task_index.find(request->task_id());
        if (it == task_manager.task_index.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                fmt::format("Task with ID = {} not found.", request->task_id()));
        } else {
            auto index = task_manager.task_index[request->task_id()];
            auto task = task_manager.tasks[index];
            if (task.state() == TaskState::Running) {
                task.state() = TaskState::Complete;
                task.output() = request->output();
            }
            return grpc::Status::OK;
        }
    }

    grpc::Status Requeue(grpc::ServerContext*, const RequeueRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        double max_start_time = now_seconds() - request->timeout_s();

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        for (std::size_t index = 0; index < std::size(task_manager.tasks); index++) {
            auto task = task_manager.tasks[index];
            if (task.state() == TaskState::Running && task.start_time() < max_start_time) {
                task.state() = TaskState::Ready;
                task.start_time() = -1;

                for (const auto& qname : task.queues()) {
                    task_manager.queue[qname].push(std::make_pair(task.priority(), index));
                }
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status JournalSize(grpc::ServerContext*, const JournalSizeRequest* request,
                             JournalSizeResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& journal_map = GLOBAL_SYSTEM_STATE->journal_map;
        auto it = journal_map.find(request->key());
        if (it == journal_map.end()) {
            response->set_size(0);
        } else {
            response->set_size(it->second.size());
        }

        return grpc::Status::OK;
    }

    grpc::Status JournalRead(grpc::ServerContext*, const JournalReadRequest* request,
                             JournalReadResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& journal_map = GLOBAL_SYSTEM_STATE->journal_map;
        auto it = journal_map.find(request->key());
        if (it != journal_map.end()) {
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

    grpc::Status JournalAppend(grpc::ServerContext*, const JournalAppendRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        GLOBAL_SYSTEM_STATE->journal_map[request->key()].push_back(request->value());
        return grpc::Status::OK;
    }

    grpc::Status TimeSeriesAppend(grpc::ServerContext*, const TimeSeriesAppendRequest* request, Empty*) override {
        auto tp = parse_iso8601_utc(request->datetime());
        if (!tp) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid ISO 8601 UTC datetime: {}", request->datetime()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto& series = GLOBAL_SYSTEM_STATE->time_series[request->key()];
        series.value.push_back(request->value());
        series.time.push_back(*tp);
        series.step.push_back(request->step());

        return grpc::Status::OK;
    }

    grpc::Status TimeSeriesGet(grpc::ServerContext*, const TimeSeriesGetRequest* request,
                               TimeSeriesGetResponse* response) override {
        // An empty time string means "no bound";
        // a non-empty one that fails to parse is an error.
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

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->lock};

        auto it = GLOBAL_SYSTEM_STATE->time_series.find(request->key());
        if (it == GLOBAL_SYSTEM_STATE->time_series.end()) {
            return grpc::Status::OK;
        }

        const auto& series = it->second;
        for (std::size_t index = 0; index < series.value.size(); index++) {
            // start bounds are inclusive, end bounds exclusive;
            // unset bounds don't filter.
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
};

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program(argv[0]);
    program.add_description("A data structure server.");

    std::string server_address{};

    // clang-format off
    program.add_argument("-a", "--address")
	.help("server address")
	.default_value(std::string{"127.0.0.1:5051"})
	.store_into(server_address);
    // clang-format on

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        spdlog::error("Failed to parse arguments: {}\n{}", err.what(), program.usage());
        std::exit(1);
    }

    spdlog::info("server_address = {}", server_address);

    SystemState global_system_state{};
    GLOBAL_SYSTEM_STATE = &global_system_state;

    DsServiceImpl service{};
    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder{};
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    // Server sends keepalive pings every 10 mins with 20 second timeout.
    // Pings will be sent even if there are no calls in flight.
    // Server with permit ping at an interval of 10 seconds.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 10 * 60 * 1000 /*10 min*/);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20 * 1000 /*20 sec*/);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 10 * 1000 /*10 sec*/);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    GLOBAL_SYSTEM_STATE->server = server.get();

    spdlog::info("starting server ...");
    server->Wait();

    return 0;
}
