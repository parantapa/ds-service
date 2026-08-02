#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <experimental/scope>

#include <spdlog/spdlog.h>
#include <argparse/argparse.hpp>
#include <parallel_hashmap/phmap.h>
#include <re2/re2.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

template <typename K, typename V>
using Map = phmap::parallel_flat_hash_map<K, V>;

using TaskQueueEntry = std::priority_queue<std::pair<double, std::size_t>>;

// Tasks are stored struct-of-arrays:
// a task is a row index shared across these parallel vectors,
// which are always the same length.
// Add a task by pushing onto every column,
// and read a field as `tasks.<column>[index]`.
struct TaskTable {
    std::vector<std::string> task_id;
    std::vector<double> priority;
    std::vector<std::string> function;
    std::vector<std::string> input;
    std::vector<std::string> output;
    std::vector<TaskState> state;
    std::vector<double> start_time;
    std::vector<std::vector<std::string>> queues;
};

struct TaskManager {
    TaskTable tasks;

    // task_id -> the row in `tasks` holding it.
    Map<std::string, std::size_t> task_index;

    // Queue name -> the rows waiting on it, ordered by priority.
    // std::priority_queue is a max-heap,
    // so the highest priority is dispatched first.
    // A row may sit in several queues at once,
    // and entries are never removed on a state change
    // -- TaskGet drops the stale ones as it pops them.
    Map<std::string, TaskQueueEntry> queue;
};

struct TimeSeries {
    std::vector<double> value;
    std::vector<std::chrono::system_clock::time_point> time;
    std::vector<std::int64_t> step;
};

// All server state, and the locks guarding it.
//
// Locking is per top-level data structure rather than one global lock:
// each structure is paired with its own mutex,
// so operations on one are serialized
// while operations on different ones run concurrently.
// Every RPC takes a std::scoped_lock on the single structure it touches,
// so no request ever holds more than one lock
// and the ordering between them cannot deadlock.
// Keep it that way:
// an RPC spanning two structures would need a lock order
// defined for the whole file.
// There is no finer-grained locking within a structure
// -- no per-key or per-queue locks --
// so a slow whole-structure scan (any SearchKey, TaskRequeue)
// blocks every other operation on that structure.
struct SystemState {
    std::mutex map_lock{};
    Map<std::string, std::string> map{};

    std::mutex journal_map_lock{};
    Map<std::string, std::vector<std::string>> journal_map{};

    std::mutex time_series_lock{};
    Map<std::string, TimeSeries> time_series{};

    std::mutex mutexes_lock{};
    Map<std::string, bool> mutexes{};

    std::mutex counters_lock{};
    Map<std::string, std::uint64_t> counters{};

    std::mutex task_manager_lock{};
    TaskManager task_manager{};

    grpc::Server* server{nullptr};

    // Written by the shutdown thread, so not a plain bool.
    std::atomic<bool> shutdown{false};
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
// Only differences between two readings are meaningful;
// the epoch is arbitrary.
// This is the only clock the task table uses:
// TaskGet stamps start_time with it
// and TaskRequeue compares against it,
// so both must keep using this function rather than any other clock.
double now_seconds() {
    using namespace std::chrono;
    return duration<double>(high_resolution_clock::now().time_since_epoch()).count();
}

SystemState* GLOBAL_SYSTEM_STATE = nullptr;

struct DsServiceImpl final : public DsService::Service {
    grpc::Status MapSet(grpc::ServerContext*, const MapSetRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->map_lock};

        GLOBAL_SYSTEM_STATE->map[request->key()] = request->value();
        return grpc::Status::OK;
    }

    grpc::Status MapGet(grpc::ServerContext*, const MapGetRequest* request, MapGetResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->map_lock};

        auto& map = GLOBAL_SYSTEM_STATE->map;
        auto it = map.find(request->key());
        if (it == map.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, fmt::format("Key {} not found.", request->key()));
        } else {
            response->set_value(it->second);
        }

        return grpc::Status::OK;
    }

    grpc::Status MapSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                              SearchKeyResponse* response) override {
        RE2 pattern{request->pattern()};
        if (!pattern.ok()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid regular expression: {}", pattern.error()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->map_lock};

        for (const auto& [key, _] : GLOBAL_SYSTEM_STATE->map) {
            if (RE2::PartialMatch(key, pattern)) {
                response->add_key(key);
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status TaskAdd(grpc::ServerContext*, const TaskAddRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->task_manager_lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto it = task_manager.task_index.find(request->task_id());
        if (it == task_manager.task_index.end()) {
            auto& tasks = task_manager.tasks;
            tasks.task_id.push_back(request->task_id());
            tasks.priority.push_back(request->priority());
            tasks.function.push_back(request->function());
            tasks.input.push_back(request->input());
            tasks.output.push_back("");
            tasks.state.push_back(TaskState::Ready);
            tasks.start_time.push_back(-1.0);
            tasks.queues.push_back({});

            auto index = tasks.task_id.size() - 1;

            task_manager.task_index[request->task_id()] = index;
            for (const auto& qname : request->queue()) {
                tasks.queues[index].push_back(qname);
                task_manager.queue[qname].push(std::make_pair(request->priority(), index));
            }

            return grpc::Status::OK;
        } else {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                                fmt::format("Task with ID = {} already exists.", request->task_id()));
        }
    }

    grpc::Status TaskGetStatus(grpc::ServerContext*, const TaskGetStatusRequest* request,
                               TaskGetStatusResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->task_manager_lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        for (const auto& task_id : request->task_id()) {
            auto it = task_manager.task_index.find(task_id);
            // An unknown task_id reports Undefined rather than being an error.
            if (it == task_manager.task_index.end()) {
                response->add_state(TaskState::Undefined);
            } else {
                response->add_state(task_manager.tasks.state[it->second]);
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status TaskGetOutput(grpc::ServerContext*, const TaskGetOutputRequest* request,
                               TaskGetOutputResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->task_manager_lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto it = task_manager.task_index.find(request->task_id());
        if (it == task_manager.task_index.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                fmt::format("Task with ID = {} not found.", request->task_id()));
        }

        response->set_output(task_manager.tasks.output[it->second]);
        return grpc::Status::OK;
    }

    grpc::Status TaskGetCountByState(grpc::ServerContext*, const Empty*,
                                     TaskGetCountByStateResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->task_manager_lock};

        // Every task occupies exactly one row in the SOA,
        // so tallying the state column gives the count per state.
        auto& tasks = GLOBAL_SYSTEM_STATE->task_manager.tasks;
        std::uint64_t ready = 0, running = 0, complete = 0;
        for (const auto& state : tasks.state) {
            switch (state) {
            case TaskState::Ready:
                ready++;
                break;
            case TaskState::Running:
                running++;
                break;
            case TaskState::Complete:
                complete++;
                break;
            default:
                break;
            }
        }

        response->set_ready(ready);
        response->set_running(running);
        response->set_complete(complete);
        return grpc::Status::OK;
    }

    grpc::Status TaskGet(grpc::ServerContext*, const TaskGetRequest* request, TaskGetResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->task_manager_lock};

        // Queues are searched in the order the caller listed them:
        // the first one holding a Ready task wins.
        //
        // A queue entry is never removed when its task leaves the Ready state,
        // so entries for tasks that are already Running or Complete accumulate.
        // They are discarded lazily here, as they reach the top of the heap
        // -- which is why a popped entry that is not Ready
        // is dropped rather than skipped.
        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto& tasks = task_manager.tasks;
        for (const auto& qname : request->queue()) {
            // find, not operator[]:
            // polling a queue no task was ever added to must not create it.
            // Workers poll queue names on a loop,
            // so operator[] here would grow the map
            // by one empty queue per name ever asked about,
            // for the life of the server.
            auto queue_it = task_manager.queue.find(qname);
            if (queue_it == task_manager.queue.end()) {
                continue;
            }

            auto& queue = queue_it->second;
            while (!queue.empty()) {
                const auto& [_, index] = queue.top();
                if (tasks.state[index] == TaskState::Ready) {
                    tasks.state[index] = TaskState::Running;
                    tasks.start_time[index] = now_seconds();

                    response->set_task_id(tasks.task_id[index]);
                    response->set_function(tasks.function[index]);
                    response->set_input(tasks.input[index]);

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
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->task_manager_lock};

        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto it = task_manager.task_index.find(request->task_id());
        if (it == task_manager.task_index.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                fmt::format("Task with ID = {} not found.", request->task_id()));
        } else {
            auto index = it->second;
            auto& tasks = task_manager.tasks;
            if (tasks.state[index] == TaskState::Running) {
                tasks.state[index] = TaskState::Complete;
                tasks.output[index] = request->output();
            }
            return grpc::Status::OK;
        }
    }

    grpc::Status TaskRequeue(grpc::ServerContext*, const TaskRequeueRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->task_manager_lock};

        // The only fault tolerance the server has:
        // a worker that dies mid-task leaves it Running for ever,
        // so a client calls this periodically
        // to hand stalled work to another worker.
        // It is never called automatically.
        double max_start_time = now_seconds() - request->timeout_s();

        // Stalled tasks are found by scanning every row
        // -- there is no index by state or by start time --
        // while holding the task manager's lock,
        // so this blocks all other task operations for as long as it runs.
        auto& task_manager = GLOBAL_SYSTEM_STATE->task_manager;
        auto& tasks = task_manager.tasks;
        for (std::size_t index = 0; index < tasks.task_id.size(); index++) {
            if (tasks.state[index] == TaskState::Running && tasks.start_time[index] < max_start_time) {
                tasks.state[index] = TaskState::Ready;
                tasks.start_time[index] = -1;

                for (const auto& qname : tasks.queues[index]) {
                    task_manager.queue[qname].push(std::make_pair(tasks.priority[index], index));
                }
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status JournalSize(grpc::ServerContext*, const JournalSizeRequest* request,
                             JournalSizeResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->journal_map_lock};

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
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->journal_map_lock};

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
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->journal_map_lock};

        GLOBAL_SYSTEM_STATE->journal_map[request->key()].push_back(request->value());
        return grpc::Status::OK;
    }

    grpc::Status JournalSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                  SearchKeyResponse* response) override {
        RE2 pattern{request->pattern()};
        if (!pattern.ok()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid regular expression: {}", pattern.error()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->journal_map_lock};

        for (const auto& [key, _] : GLOBAL_SYSTEM_STATE->journal_map) {
            if (RE2::PartialMatch(key, pattern)) {
                response->add_key(key);
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status TimeSeriesAppend(grpc::ServerContext*, const TimeSeriesAppendRequest* request, Empty*) override {
        auto tp = parse_iso8601_utc(request->datetime());
        if (!tp) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid ISO 8601 UTC datetime: {}", request->datetime()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->time_series_lock};

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

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->time_series_lock};

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

    grpc::Status TimeSeriesSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                     SearchKeyResponse* response) override {
        RE2 pattern{request->pattern()};
        if (!pattern.ok()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid regular expression: {}", pattern.error()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->time_series_lock};

        for (const auto& [key, _] : GLOBAL_SYSTEM_STATE->time_series) {
            if (RE2::PartialMatch(key, pattern)) {
                response->add_key(key);
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status MutexTryAcquire(grpc::ServerContext*, const MutexTryAcquireRequest* request,
                                 MutexTryAcquireResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->mutexes_lock};

        // operator[] value-initializes a missing mutex to false (unheld),
        // so an unknown key is created and then acquired by this same call.
        bool& held = GLOBAL_SYSTEM_STATE->mutexes[request->key()];
        if (held) {
            response->set_acquired(false);
        } else {
            held = true;
            response->set_acquired(true);
        }

        return grpc::Status::OK;
    }

    grpc::Status MutexRelease(grpc::ServerContext*, const MutexReleaseRequest* request, Empty*) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->mutexes_lock};

        // Releasing an unheld or unknown mutex is a no-op;
        // don't create the key.
        auto& mutexes = GLOBAL_SYSTEM_STATE->mutexes;
        auto it = mutexes.find(request->key());
        if (it != mutexes.end()) {
            it->second = false;
        }

        return grpc::Status::OK;
    }

    grpc::Status MutexSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                SearchKeyResponse* response) override {
        RE2 pattern{request->pattern()};
        if (!pattern.ok()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid regular expression: {}", pattern.error()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->mutexes_lock};

        for (const auto& [key, _] : GLOBAL_SYSTEM_STATE->mutexes) {
            if (RE2::PartialMatch(key, pattern)) {
                response->add_key(key);
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status CounterGetNextValue(grpc::ServerContext*, const CounterGetNextValueRequest* request,
                                     CounterGetNextValueResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->counters_lock};

        // operator[] value-initializes a missing counter to 0,
        // so pre-incrementing makes the first call return 1
        // and creates the counter.
        std::uint64_t& counter = GLOBAL_SYSTEM_STATE->counters[request->key()];
        response->set_value(++counter);

        return grpc::Status::OK;
    }

    grpc::Status CounterGetCurrentValue(grpc::ServerContext*, const CounterGetCurrentValueRequest* request,
                                        CounterGetCurrentValueResponse* response) override {
        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->counters_lock};

        // Read-only: don't create a missing counter;
        // report 0 for one that does not exist.
        auto& counters = GLOBAL_SYSTEM_STATE->counters;
        auto it = counters.find(request->key());
        response->set_value(it == counters.end() ? 0 : it->second);

        return grpc::Status::OK;
    }

    grpc::Status CounterSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                  SearchKeyResponse* response) override {
        RE2 pattern{request->pattern()};
        if (!pattern.ok()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                fmt::format("Invalid regular expression: {}", pattern.error()));
        }

        std::scoped_lock lock{GLOBAL_SYSTEM_STATE->counters_lock};

        for (const auto& [key, _] : GLOBAL_SYSTEM_STATE->counters) {
            if (RE2::PartialMatch(key, pattern)) {
                response->add_key(key);
            }
        }

        return grpc::Status::OK;
    }
};

// Largest single request or response accepted, in bytes.
// gRPC's default is 4 MiB.
// The Python client sets the same limit;
// the two must be changed together,
// or one side rejects what the other sends.
// tests/test_grpc_options.py checks that they still agree.
constexpr int MAX_MESSAGE_SIZE_BYTES = 64 * 1024 * 1024;

// How long in-flight RPCs are given to finish once shutdown starts.
// Anything still running when the deadline passes is cancelled.
constexpr int SHUTDOWN_GRACE_S = 5;

const char* VERSION = "2.2.0";

// How often the thread below looks for a delivered signal.
// It bounds how long shutdown takes to start, so keep it short.
constexpr auto SHUTDOWN_POLL_INTERVAL = std::chrono::milliseconds(100);

// Set by the signal handler, read by the shutdown thread.
volatile std::sig_atomic_t SHUTDOWN_SIGNAL = 0;

// A signal handler may touch nothing but a volatile sig_atomic_t.
// It records the signal and leaves the work to the thread below.
// Calling Shutdown(), or logging, from here would be undefined behaviour.
extern "C" void handle_shutdown_signal(int signum) {
    SHUTDOWN_SIGNAL = signum;
}

// Wait for SIGINT or SIGTERM, then shut the server down gracefully.
//
// Shutdown() refuses new calls,
// lets in-flight ones finish until the deadline,
// and makes the Wait() in main return.
void await_shutdown_signal() {
    while (SHUTDOWN_SIGNAL == 0) {
        std::this_thread::sleep_for(SHUTDOWN_POLL_INTERVAL);
    }

    spdlog::info("received signal {}; shutting down ...", static_cast<int>(SHUTDOWN_SIGNAL));

    GLOBAL_SYSTEM_STATE->shutdown = true;
    GLOBAL_SYSTEM_STATE->server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(SHUTDOWN_GRACE_S));
}

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program(argv[0], VERSION);
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

    // Installed before the server starts,
    // so a signal arriving during startup is recorded
    // and acted on as soon as the shutdown thread runs.
    if (std::signal(SIGINT, handle_shutdown_signal) == SIG_ERR ||
        std::signal(SIGTERM, handle_shutdown_signal) == SIG_ERR) {
        spdlog::error("Failed to install shutdown signal handlers");
        return 1;
    }

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
    //
    // That last one is a floor on how often a *client* may ping:
    // the client's keepalive_time_ms (120s in client.py) must stay above it,
    // or the server answers the pings with GOAWAY/ENHANCE_YOUR_CALM
    // and kills every long-lived connection
    // -- which reaches callers as a TimeoutError
    // that says nothing about pings.
    // tests/test_grpc_options.py checks the two stay ordered.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 10 * 60 * 1000 /*10 min*/);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20 * 1000 /*20 sec*/);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 10 * 1000 /*10 sec*/);

    // Refuse to share the port.
    // gRPC enables SO_REUSEPORT by default,
    // so without this
    // a second ds-service started on an occupied address
    // binds silently alongside the first.
    // State is in-memory and non-persistent,
    // so that splits clients across two divergent instances
    // instead of failing.
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);

    builder.SetMaxReceiveMessageSize(MAX_MESSAGE_SIZE_BYTES);
    builder.SetMaxSendMessageSize(MAX_MESSAGE_SIZE_BYTES);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        // BuildAndStart returns null when the port cannot be bound.
        // Report it instead of dereferencing null below.
        spdlog::error("Failed to bind {}; is another ds-service already running there?", server_address);
        return 1;
    }
    GLOBAL_SYSTEM_STATE->server = server.get();

    // Started only once the server pointer is published,
    // which is what this thread calls Shutdown() on.
    std::thread shutdown_thread{await_shutdown_signal};

    spdlog::info("starting server ...");
    server->Wait();
    shutdown_thread.join();
    spdlog::info("server stopped");

    return 0;
}
