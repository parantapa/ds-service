#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <signal.h>

#include <spdlog/spdlog.h>
#include <argparse/argparse.hpp>

#include "core/system-state.hpp"
#include "server/grpc-server-transport.hpp"
#include "transport.hpp"

using Transports = std::vector<std::unique_ptr<ServerTransport>>;

// How long in-flight calls are given to finish once shutdown starts.
// It stays below TERMINATE_TIMEOUT_S in python/ds_service_client/server.py,
// after which DsServiceServer kills a server that has not exited.
constexpr int SHUTDOWN_GRACE_S = 5;

// scripts/update-version.sh rewrites this line.
// See "Versioning" in docs/developer-notes.md.
const char* VERSION = "7.0.0";

// The signals that start a graceful shutdown.
sigset_t shutdown_signals() {
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Wait for SIGINT or SIGTERM, then shut every transport down gracefully.
//
// main blocks both signals in every thread,
// so they stay pending until sigwait takes them here.
// No signal handler runs,
// so this thread is free to log and call shutdown().
// Every transport shares one deadline.
void await_shutdown_signal(SystemState& state, const Transports& transports) {
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

    state.shutdown = true;
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(SHUTDOWN_GRACE_S);
    for (const auto& transport : transports) {
        transport->shutdown(deadline);
    }
}

int main(int argc, char* argv[]) {
    // main blocks the shutdown signals first, before any transport starts a thread.
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

    SystemState state{};

    Transports transports{};
    transports.push_back(std::make_unique<GrpcServerTransport>(server_address));

    // A transport that fails to start has logged why.
    // The ones already started stop when main returns and destroys them.
    for (const auto& transport : transports) {
        if (!transport->start(state)) {
            return 1;
        }
        spdlog::info("started transport {}", transport->name());
    }

    // This thread calls shutdown() on every transport.
    // Start it only after every transport has started,
    // because a start failure returns from main,
    // and destroying a joinable std::thread calls std::terminate.
    std::thread shutdown_thread{await_shutdown_signal, std::ref(state), std::cref(transports)};

    spdlog::info("starting server ...");
    for (const auto& transport : transports) {
        transport->wait();
    }
    shutdown_thread.join();
    spdlog::info("server stopped");

    return 0;
}
