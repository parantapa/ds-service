// A smoke test of the C++ client against a real server.
//
// Usage: client-smoke PATH_TO_DS_SERVICE
//
// Starts the server on a free loopback port,
// makes one call per data structure,
// and checks the error each failure path throws.
// The Python suite in tests/ is the full behavioral suite.
// This test only shows that the C++ client reaches the server
// and reports failures with the right ErrorCode.
// Exits 0 when every check passes, and 1 otherwise.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ds-service/connect.hpp"

extern char** environ;

namespace {

using namespace std::chrono_literals;

constexpr auto STARTUP_TIMEOUT = 15s;
constexpr auto STARTUP_POLL_INTERVAL = 50ms;

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        failures++;
    } else {
        std::cerr << "ok:   " << what << "\n";
    }
}

// Run body, and check that it throws ClientError with the expected code.
void check_throws(ds::ErrorCode expected, const std::string& what, const std::function<void()>& body) {
    try {
        body();
        check(false, what + " (nothing thrown)");
    } catch (const ds::ClientError& error) {
        check(error.code() == expected,
              what + " (code " + std::to_string(static_cast<int>(error.code())) + ": " + error.what() + ")");
    }
}

// A TCP socket bound to a free loopback port, optionally listening.
// The kernel completes the handshake from the listen backlog,
// and nothing ever calls accept(), so a listening socket never answers a call.
struct LoopbackSocket {
    int fd = -1;
    int port = 0;

    explicit LoopbackSocket(bool listening) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            (listening && ::listen(fd, 8) != 0)) {
            std::cerr << "cannot bind a loopback socket\n";
            std::exit(1);
        }
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }

    ~LoopbackSocket() {
        close();
    }

    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    std::string address() const {
        return "127.0.0.1:" + std::to_string(port);
    }
};

// The server under test, on a free port, stopped with SIGTERM.
struct Server {
    pid_t pid = -1;
    std::string address;

    explicit Server(const std::string& binary) {
        // The probe socket is closed before the server binds the port.
        // Nothing holds the port in between, and the kernel is only unlikely to reuse it.
        LoopbackSocket probe{false};
        address = probe.address();
        probe.close();

        std::vector<std::string> args{binary, "--address", address};
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        if (posix_spawn(&pid, binary.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
            std::cerr << "cannot start " << binary << "\n";
            std::exit(1);
        }
    }

    // SIGTERM, then the exit status, which is 0 after a graceful shutdown.
    int stop() {
        ::kill(pid, SIGTERM);
        int status = 0;
        ::waitpid(pid, &status, 0);
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    ~Server() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
    }
};

// Block until the server answers a read-only call.
void wait_until_ready(const std::string& address) {
    const auto deadline = std::chrono::steady_clock::now() + STARTUP_TIMEOUT;
    auto client = ds::connect(address, {.timeout = 1s});
    while (true) {
        try {
            client->counter_get_current_value("probe");
            return;
        } catch (const ds::ClientError& error) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::cerr << "server at " << address << " never answered: " << error.what() << "\n";
                std::exit(1);
            }
            std::this_thread::sleep_for(STARTUP_POLL_INTERVAL);
        }
    }
}

void check_data_structures(ds::Client& client) {
    // Binary payloads survive the round trip.
    const std::string payload{"\0\xff\x01payload", 10};
    client.map_set("k", payload);
    check(client.map_get("k") == payload, "map_set then map_get");
    check(client.map_search_key("^k$") == std::vector<std::string>{"k"}, "map_search_key");

    client.task_add("t1", {}, {"q"}, 1.0, "fn", "in");
    auto task = client.task_get("w1", {"q"});
    check(task.task_id == "t1" && task.function == "fn" && task.input == "in", "task_add then task_get");
    client.task_done("t1", "w1", "out");
    check(client.task_get_output("t1") == "out", "task_done then task_get_output");
    check(client.task_get_status({"t1", "ghost"}) ==
              std::vector<ds::TaskState>{ds::TaskState::Finished, ds::TaskState::Undefined},
          "task_get_status");
    check(client.task_get_count_by_state().finished == 1, "task_get_count_by_state");

    client.journal_append("j", "a");
    client.journal_append("j", "b");
    check(client.journal_size("j") == 2, "journal_size");
    check(client.journal_read("j", 0, 10) == std::vector<std::string>{"a", "b"}, "journal_read");

    client.time_series_append("s", 1.5, "2024-01-02T03:04:05Z", 7);
    auto points = client.time_series_get("s", std::nullopt, std::nullopt, 7);
    check(points.size() == 1 && points[0].value == 1.5 && points[0].datetime == "2024-01-02T03:04:05Z" &&
              points[0].step == 7,
          "time_series_append then time_series_get");

    check(client.mutex_try_acquire("m", "w1"), "mutex_try_acquire");
    check(client.mutex_get_worker_id("m") == "w1", "mutex_get_worker_id");
    client.mutex_release("m", "w1");
    client.mutex_acquire("m", "w2", 1s);
    check(client.mutex_get_worker_id("m") == "w2", "mutex_acquire");

    check(client.counter_get_next_value("c") == 1, "counter_get_next_value");
    check(client.counter_get_current_value("c") == 1, "counter_get_current_value");
}

void check_refusals(ds::Client& client) {
    check_throws(ds::ErrorCode::NotFound, "map_get on a missing key", [&] { client.map_get("missing"); });
    check_throws(ds::ErrorCode::NotFound, "task_get on an empty queue", [&] { client.task_get("w1", {"empty"}); });
    check_throws(ds::ErrorCode::AlreadyExists, "task_add of a known id",
                 [&] { client.task_add("t1", {}, {"q"}, 1.0, "", ""); });
    check_throws(ds::ErrorCode::InvalidArgument, "map_search_key with a bad pattern",
                 [&] { client.map_search_key("("); });
    check_throws(ds::ErrorCode::FailedPrecondition, "mutex_release by a non-holder",
                 [&] { client.mutex_release("m", "w1"); });
    check_throws(ds::ErrorCode::DeadlineExceeded, "mutex_acquire past its timeout",
                 [&] { client.mutex_acquire("m", "w1", 100ms); });
    check_throws(ds::ErrorCode::MessageTooLarge, "map_set of an oversized value",
                 [&] { client.map_set("big", std::string(65 * 1024 * 1024, 'x')); });
}

void check_transport_failures() {
    LoopbackSocket mute{true};

    {
        auto client = ds::connect(mute.address(), {.timeout = 300ms});
        const auto start = std::chrono::steady_clock::now();
        check_throws(ds::ErrorCode::DeadlineExceeded, "a call to a server that never answers",
                     [&] { client->map_get("k"); });
        check(std::chrono::steady_clock::now() - start < 5s, "the deadline cut the call off");
    }

    {
        const auto start = std::chrono::steady_clock::now();
        auto client = ds::connect(
            mute.address(), {
                                .timeout = 60s,
                                .should_cancel = [start] { return std::chrono::steady_clock::now() - start > 200ms; },
                            });
        check_throws(ds::ErrorCode::Cancelled, "a call that should_cancel cancels", [&] { client->map_get("k"); });
        check(std::chrono::steady_clock::now() - start < 5s, "should_cancel cut the call off");
    }

    {
        auto client = ds::connect(mute.address(), {.timeout = 60s});
        std::optional<ds::ErrorCode> code;
        std::thread caller{[&] {
            try {
                client->map_get("k");
            } catch (const ds::ClientError& error) {
                code = error.code();
            }
        }};
        std::this_thread::sleep_for(200ms);
        client->close();
        caller.join();
        check(code == ds::ErrorCode::Closed, "close() during a call");
    }

    {
        LoopbackSocket dead{false};
        const auto address = dead.address();
        dead.close();
        auto client = ds::connect(address, {.timeout = 5s});
        check_throws(ds::ErrorCode::Unavailable, "a call to an address nothing listens on",
                     [&] { client->map_get("k"); });
    }

    check_throws(ds::ErrorCode::InvalidArgument, "connect with an unknown scheme",
                 [] { ds::connect("carrier-pigeon://127.0.0.1:1"); });
    check_throws(ds::ErrorCode::InvalidArgument, "connect with an empty address", [] { ds::connect(""); });
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " PATH_TO_DS_SERVICE\n";
        return 2;
    }

    Server server{argv[1]};
    wait_until_ready(server.address);

    {
        auto client = ds::connect("grpc://" + server.address);
        check_data_structures(*client);
        check_refusals(*client);

        client->close();
        check_throws(ds::ErrorCode::Closed, "a call after close()", [&] { client->map_get("k"); });
        client->close();
    }

    check_transport_failures();

    check(server.stop() == 0, "the server exits 0 on SIGTERM");

    std::cerr << (failures == 0 ? "all checks passed\n" : std::to_string(failures) + " checks failed\n");
    return failures == 0 ? 0 : 1;
}
