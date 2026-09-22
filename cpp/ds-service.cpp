#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>
#include <argparse/argparse.hpp>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service.hpp"

struct SystemState {
    Map map{};

    JournalMap journal_map{};

    TimeSeriesMap time_series{};

    Mutexes mutexes{};

    Counters counters{};

    TaskManager task_manager{};

    grpc::Server* server{nullptr};

    // Written by the shutdown thread, so not a plain bool.
    std::atomic<bool> shutdown{false};
};

SystemState* GLOBAL_SYSTEM_STATE = nullptr;

// Each method here does nothing but hand the call
// to the data structure that owns the state.
// The locking and the logic live on that structure.
struct DsServiceImpl final : public DsService::Service {
    grpc::Status MapSet(grpc::ServerContext*, const MapSetRequest* request, Empty* response) override {
        return GLOBAL_SYSTEM_STATE->map.set(request, response);
    }

    grpc::Status MapGet(grpc::ServerContext*, const MapGetRequest* request, MapGetResponse* response) override {
        return GLOBAL_SYSTEM_STATE->map.get(request, response);
    }

    grpc::Status MapSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                              SearchKeyResponse* response) override {
        return GLOBAL_SYSTEM_STATE->map.search_key(request, response);
    }

    grpc::Status TaskAdd(grpc::ServerContext*, const TaskAddRequest* request, Empty* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.add(request, response);
    }

    grpc::Status TaskGetStatus(grpc::ServerContext*, const TaskGetStatusRequest* request,
                               TaskGetStatusResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.get_status(request, response);
    }

    grpc::Status TaskGetOutput(grpc::ServerContext*, const TaskGetOutputRequest* request,
                               TaskGetOutputResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.get_output(request, response);
    }

    grpc::Status TaskGetCountByState(grpc::ServerContext*, const Empty* request,
                                     TaskGetCountByStateResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.get_count_by_state(request, response);
    }

    grpc::Status TaskCancel(grpc::ServerContext*, const TaskCancelRequest* request,
                            TaskCancelResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.cancel(request, response);
    }

    grpc::Status TaskGetPriority(grpc::ServerContext*, const TaskGetPriorityRequest* request,
                                 TaskGetPriorityResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.get_priority(request, response);
    }

    grpc::Status TaskSetPriority(grpc::ServerContext*, const TaskSetPriorityRequest* request,
                                 Empty* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.set_priority(request, response);
    }

    grpc::Status TaskGetWorkerId(grpc::ServerContext*, const TaskGetWorkerIdRequest* request,
                                 TaskGetWorkerIdResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.get_worker_id(request, response);
    }

    grpc::Status TaskSearchId(grpc::ServerContext*, const SearchKeyRequest* request,
                              SearchKeyResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.search_id(request, response);
    }

    grpc::Status TaskGet(grpc::ServerContext*, const TaskGetRequest* request, TaskGetResponse* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.get(request, response);
    }

    grpc::Status TaskDone(grpc::ServerContext*, const TaskDoneRequest* request, Empty* response) override {
        return GLOBAL_SYSTEM_STATE->task_manager.done(request, response);
    }

    grpc::Status JournalSize(grpc::ServerContext*, const JournalSizeRequest* request,
                             JournalSizeResponse* response) override {
        return GLOBAL_SYSTEM_STATE->journal_map.size(request, response);
    }

    grpc::Status JournalRead(grpc::ServerContext*, const JournalReadRequest* request,
                             JournalReadResponse* response) override {
        return GLOBAL_SYSTEM_STATE->journal_map.read(request, response);
    }

    grpc::Status JournalAppend(grpc::ServerContext*, const JournalAppendRequest* request, Empty* response) override {
        return GLOBAL_SYSTEM_STATE->journal_map.append(request, response);
    }

    grpc::Status JournalSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                  SearchKeyResponse* response) override {
        return GLOBAL_SYSTEM_STATE->journal_map.search_key(request, response);
    }

    grpc::Status TimeSeriesAppend(grpc::ServerContext*, const TimeSeriesAppendRequest* request,
                                  Empty* response) override {
        return GLOBAL_SYSTEM_STATE->time_series.append(request, response);
    }

    grpc::Status TimeSeriesGet(grpc::ServerContext*, const TimeSeriesGetRequest* request,
                               TimeSeriesGetResponse* response) override {
        return GLOBAL_SYSTEM_STATE->time_series.get(request, response);
    }

    grpc::Status TimeSeriesSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                     SearchKeyResponse* response) override {
        return GLOBAL_SYSTEM_STATE->time_series.search_key(request, response);
    }

    grpc::Status MutexTryAcquire(grpc::ServerContext*, const MutexTryAcquireRequest* request,
                                 MutexTryAcquireResponse* response) override {
        return GLOBAL_SYSTEM_STATE->mutexes.try_acquire(request, response);
    }

    grpc::Status MutexRelease(grpc::ServerContext*, const MutexReleaseRequest* request, Empty* response) override {
        return GLOBAL_SYSTEM_STATE->mutexes.release(request, response);
    }

    grpc::Status MutexGetWorkerId(grpc::ServerContext*, const MutexGetWorkerIdRequest* request,
                                  MutexGetWorkerIdResponse* response) override {
        return GLOBAL_SYSTEM_STATE->mutexes.get_worker_id(request, response);
    }

    grpc::Status MutexSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                SearchKeyResponse* response) override {
        return GLOBAL_SYSTEM_STATE->mutexes.search_key(request, response);
    }

    grpc::Status CounterGetNextValue(grpc::ServerContext*, const CounterGetNextValueRequest* request,
                                     CounterGetNextValueResponse* response) override {
        return GLOBAL_SYSTEM_STATE->counters.get_next_value(request, response);
    }

    grpc::Status CounterGetCurrentValue(grpc::ServerContext*, const CounterGetCurrentValueRequest* request,
                                        CounterGetCurrentValueResponse* response) override {
        return GLOBAL_SYSTEM_STATE->counters.get_current_value(request, response);
    }

    grpc::Status CounterSearchKey(grpc::ServerContext*, const SearchKeyRequest* request,
                                  SearchKeyResponse* response) override {
        return GLOBAL_SYSTEM_STATE->counters.search_key(request, response);
    }
};

// Largest single request or response accepted, in bytes.
// gRPC's default is 4 MiB.
// The Python client sets the same limit, and the two must agree.
// tests/test_grpc_options.py checks that they still do.
// See "The channel settings are one setting in two languages"
// in docs/developer-notes.md.
constexpr int MAX_MESSAGE_SIZE_BYTES = 64 * 1024 * 1024;

// How long in-flight RPCs are given to finish once shutdown starts.
// The server cancels anything still running when the deadline passes.
constexpr int SHUTDOWN_GRACE_S = 5;

const char* VERSION = "5.2.0";

// How often await_shutdown_signal looks for a delivered signal.
// It bounds how long shutdown takes to start, so keep it short.
constexpr auto SHUTDOWN_POLL_INTERVAL = std::chrono::milliseconds(100);

// Set by the signal handler, read by the shutdown thread.
volatile std::sig_atomic_t SHUTDOWN_SIGNAL = 0;

// A signal handler must touch nothing but a volatile sig_atomic_t.
// It records the signal and leaves the work to await_shutdown_signal.
// A call to Shutdown(), or a log write, from here is undefined behavior.
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

    // main installs the handlers before the server starts.
    // The handler records a signal that arrives during startup,
    // and the shutdown thread acts on it as soon as it runs.
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

    // Keepalive, in the order of the AddChannelArgument calls:
    // - Ping an idle connection every 10 minutes.
    // - Give each ping 20 seconds to be answered.
    // - Keep pinging even with no calls in flight.
    //
    // The fourth argument is different in kind:
    // it is a floor on how often a client can ping.
    // The client's keepalive_time_ms (120 seconds in client.py)
    // must stay above it.
    // tests/test_grpc_options.py checks the two stay ordered.
    // See "The channel settings are one setting in two languages"
    // in docs/developer-notes.md.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 10 * 60 * 1000 /*10 minutes*/);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20 * 1000 /*20 seconds*/);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 10 * 1000 /*10 seconds*/);

    // Refuse to share the port,
    // which gRPC otherwise allows and nothing reports.
    // See "The server refuses to share its port"
    // in docs/developer-notes.md.
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

    // This thread calls Shutdown() on the server pointer.
    // Start it only after main publishes that pointer.
    std::thread shutdown_thread{await_shutdown_signal};

    spdlog::info("starting server ...");
    server->Wait();
    shutdown_thread.join();
    spdlog::info("server stopped");

    return 0;
}
