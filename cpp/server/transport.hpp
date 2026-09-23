#pragma once

#include <chrono>
#include <string_view>

struct SystemState;

// A way for clients to reach the server's state, such as gRPC.
//
// main starts every transport before it waits on any of them.
// The shutdown thread calls shutdown() on each while main is in wait().
// So shutdown() must be safe to call from another thread during wait().
class ServerTransport {
  public:
    virtual ~ServerTransport() = default;

    // A short name for the logs.
    virtual std::string_view name() const = 0;

    // Bind and start serving state.
    // state must outlive the transport.
    // Returns false on a bind failure, after logging it.
    virtual bool start(SystemState& state) = 0;

    // Block until shutdown() has completed.
    virtual void wait() = 0;

    // Refuse new calls, give in-flight calls until deadline to finish,
    // and cancel any still running then.
    virtual void shutdown(std::chrono::system_clock::time_point deadline) = 0;
};
