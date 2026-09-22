#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <pthread.h>
#include <signal.h>

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

// main points this at its own SystemState before the server starts,
// and every DsServiceImpl method and the shutdown thread reach the state through it.
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

// Largest single request or response accepted.
// gRPC's default is 4 MiB.
// client.py holds the same value.
// See "The channel settings are one setting in two languages"
// in docs/developer-notes.md.
constexpr int MAX_MESSAGE_SIZE_BYTES = 64 * 1024 * 1024;

// How long in-flight RPCs are given to finish once shutdown starts.
// The server cancels anything still running when the deadline passes.
constexpr int SHUTDOWN_GRACE_S = 5;

// scripts/update-version.sh rewrites this line.
// See "Versioning" in docs/developer-notes.md.
const char* VERSION = "6.0.0";

// The signals that start a graceful shutdown.
sigset_t shutdown_signals() {
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Wait for SIGINT or SIGTERM, then shut the server down gracefully.
//
// main blocks both signals in every thread,
// so they stay pending until sigwait takes them here.
// No signal handler runs,
// so this thread is free to log and call Shutdown().
//
// Shutdown() refuses new calls,
// lets in-flight ones finish until the deadline,
// and makes the Wait() in main return.
void await_shutdown_signal() {
    const sigset_t signals = shutdown_signals();
    int signum = 0;

    // sigwait fails only on an invalid signal set.
    // The server then shuts down at once
    // rather than run on with no way to stop it but SIGKILL.
    if (const int err = sigwait(&signals, &signum); err != 0) {
        spdlog::error("sigwait failed with error {}; shutting down ...", err);
    } else {
        spdlog::info("received signal {}; shutting down ...", signum);
    }

    GLOBAL_SYSTEM_STATE->shutdown = true;
    GLOBAL_SYSTEM_STATE->server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(SHUTDOWN_GRACE_S));
}

int main(int argc, char* argv[]) {
    // main blocks the shutdown signals first, before gRPC starts any thread.
    // A new thread inherits the signal mask of the thread that creates it,
    // so no thread takes the default action and kills the process.
    // A signal that arrives during startup stays pending,
    // and the shutdown thread takes it as soon as it runs.
    // A signal that arrives before main still kills the process.
    const sigset_t signals = shutdown_signals();
    if (const int err = pthread_sigmask(SIG_BLOCK, &signals, nullptr); err != 0) {
        spdlog::error("Failed to block shutdown signals: error {}", err);
        return 1;
    }

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

    SystemState global_system_state{};
    GLOBAL_SYSTEM_STATE = &global_system_state;

    DsServiceImpl service{};
    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder{};
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    // The fourth argument is different in kind from the first three:
    // it is a floor on how often a client can ping,
    // and the client's keepalive interval in client.py must stay above it.
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
