# C++ client reference

`ds::Client` is the C++23 client for `ds-service`.
It presents the server's data structures as ordinary methods,
like the [Python client](python-client.md), which is built on it.

This document describes the C++ library.
For what each underlying RPC does, see
the [data structure reference](data-structure.md).
For how to build it, see
[how to build the server](../how-to-guides/build-the-server.md),
which builds the client libraries as well.

## Headers and libraries

| Header | Declares |
| --- | --- |
| `ds-service/connect.hpp` | `ds::connect()`. It includes `client.hpp`. |
| `ds-service/client.hpp` | `ds::Client`. |
| `ds-service/client-transport.hpp` | `ds::ClientOptions`, and the `ds::ClientTransport` interface. |
| `ds-service/messages.hpp` | The message types some methods return, such as `ds::TaskGetResponse`. |
| `ds-service/task-state.hpp` | `ds::TaskState`. |
| `ds-service/error.hpp` | `ds::ClientError` and `ds::ErrorCode`. |

A program links the CMake target `ds-service-connect`,
which brings in `ds-service-client` and every transport.
The install step installs neither these libraries nor their headers,
so the target exists only inside this repository's CMake build.
No public header includes a gRPC or protobuf header.

## `ds::connect`

```cpp
#include <ds-service/connect.hpp>

auto client = ds::connect("127.0.0.1:5051");
```

```cpp
std::unique_ptr<ds::Client> connect(std::string_view address, ds::ClientOptions options = {});
```

`address` is `<host>:<port>`, or `grpc://<host>:<port>`.
Both select gRPC, the only transport today.
`connect` does not contact the server.
An unreachable server fails the first call instead.
It throws `ds::ClientError` with `ErrorCode::InvalidArgument`
for an empty address, or for a scheme that names no transport.

`ds::ClientOptions` has two fields:

| Field | Default | Meaning |
| --- | --- | --- |
| `timeout` | 5 minutes | The deadline of every call, as a `std::chrono::duration<double>` in seconds. |
| `should_cancel` | empty | A `std::function<bool()>` that a waiting call polls about every 100 ms, from the calling thread. When it returns true, the call is canceled and throws `ErrorCode::Cancelled`. |

## Methods

Every method is the snake_case form of the RPC it calls
(`MapSet` -> `client->map_set`),
with one addition, `mutex_acquire`.
Keys, ids, patterns and payloads are `std::string`.
A payload can hold arbitrary bytes.

| Method | Returns |
| --- | --- |
| `map_set(key, value)` | nothing |
| `map_get(key)` | `std::string` |
| `map_search_key(pattern)` | `std::vector<std::string>` |
| `task_add(task_id, parent_task_ids, queue, priority, function, input)` | nothing |
| `task_get_status(task_id)` | `std::vector<ds::TaskState>`, one per id, in the same order |
| `task_get_output(task_id)` | `std::string` |
| `task_get_count_by_state()` | `ds::TaskGetCountByStateResponse` |
| `task_cancel(task_id)` | `bool`, true if this call moved the task to `Canceled` |
| `task_get_priority(task_id)` | `double` |
| `task_set_priority(task_id, priority)` | nothing |
| `task_get_worker_id(task_id)` | `std::string` |
| `task_search_id(pattern)` | `std::vector<std::string>` |
| `task_get(worker_id, queue)` | `ds::TaskGetResponse` |
| `task_done(task_id, worker_id, output, failed = false)` | nothing |
| `journal_size(key)` | `std::uint64_t` |
| `journal_read(key, start, end)` | `std::vector<std::string>` |
| `journal_append(key, value)` | nothing |
| `journal_search_key(pattern)` | `std::vector<std::string>` |
| `time_series_append(key, value, datetime, step = 0)` | nothing |
| `time_series_get(key, start_time, end_time, start_step, end_step)` | `std::vector<ds::TimeSeriesDataPoint>` |
| `time_series_search_key(pattern)` | `std::vector<std::string>` |
| `mutex_try_acquire(key, worker_id)` | `bool`, true if this call acquired the mutex |
| `mutex_release(key, worker_id)` | nothing |
| `mutex_get_worker_id(key)` | `std::string` |
| `mutex_search_key(pattern)` | `std::vector<std::string>` |
| `mutex_acquire(key, worker_id, timeout = std::nullopt)` | nothing |
| `counter_get_next_value(key)` | `std::uint64_t` |
| `counter_get_current_value(key)` | `std::uint64_t` |
| `counter_search_key(pattern)` | `std::vector<std::string>` |
| `close()` | nothing |

The four bounds of `time_series_get` are `std::optional`,
and `std::nullopt`, their default, imposes no bound.

`mutex_acquire` retries `mutex_try_acquire` about every half second.
With a `timeout`, it throws `ErrorCode::DeadlineExceeded` once the timeout elapses,
sleeps included.
With `std::nullopt`, it retries forever.
`should_cancel` can cancel each attempt, but the sleeps between attempts do not poll it.

`close()` cancels every call in flight,
and every later call throws `ErrorCode::Closed`.
A second `close()` does nothing.

## Errors

Every method throws `ds::ClientError` on failure.
It derives from `std::runtime_error`,
`what()` holds the message,
and `code()` returns a `ds::ErrorCode`:

| `ErrorCode` | Meaning |
| --- | --- |
| `NotFound` | The key, task or mutex does not exist, or `task_get` found no task ready. |
| `AlreadyExists` | `task_add` named a task id that is already known. |
| `InvalidArgument` | A bad regular expression or datetime, or a bad address given to `connect`. |
| `FailedPrecondition` | The state of a task, or the holder of a mutex, refuses the operation. |
| `MessageTooLarge` | A request or response larger than 64 MiB. |
| `Unavailable` | The server cannot be reached. |
| `DeadlineExceeded` | The call outlived its deadline, or `mutex_acquire` its timeout. |
| `Cancelled` | `should_cancel` canceled the call, or the server canceled it. |
| `Closed` | The client is closed. |
| `Transport` | Any other failure. The message is the transport's own. |

The [data structure reference](data-structure.md) states
which RPC returns which of the first four, and when.

## Threads and processes

A client is safe to use from several threads at once.
Destroying it while another thread is in a call is not.

A client does not survive `fork()`.
Once a process has created a client,
a call from a child it forks hangs,
on an inherited client and on a new one alike.
The hang does not occur when the first client is created after forking,
or when worker processes start with `exec`.

## Example

```cpp
#include <iostream>

#include <ds-service/connect.hpp>

int main() {
    auto client = ds::connect("127.0.0.1:5051", {.timeout = std::chrono::seconds(30)});

    client->map_set("greeting", "hello");
    std::cout << client->map_get("greeting") << "\n";

    client->task_add("job-1", {}, {"work"}, 1.0, "greet", "world");
    try {
        auto task = client->task_get("worker-a", {"work"});
        client->task_done(task.task_id, "worker-a", "hello world");
    } catch (const ds::ClientError& error) {
        if (error.code() != ds::ErrorCode::NotFound) {
            throw;
        }
        // No task was ready.
    }
}
```
