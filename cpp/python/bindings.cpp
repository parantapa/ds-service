// The nanobind module ds_service_client._ext.
//
// It binds the C++ client and the message types it returns,
// and nothing else.
// python/ds_service_client/client.py builds the public Python API on top of it.
//
// Every method that makes a call follows the same three steps:
// convert the arguments to C++ with the GIL held,
// make the call with the GIL released,
// and convert the result back once the GIL is taken again.
// A payload is bytes on the Python side, never str,
// because the default std::string caster decodes UTF-8
// and a payload can hold arbitrary binary data.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include "ds-service/connect.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

std::string from_bytes(const nb::bytes& value) {
    return {value.c_str(), value.size()};
}

nb::bytes to_bytes(const std::string& value) {
    return nb::bytes(value.data(), value.size());
}

nb::list to_bytes_list(const std::vector<std::string>& values) {
    nb::list out;
    for (const auto& value : values) {
        out.append(to_bytes(value));
    }
    return out;
}

// Raise the Python error that should_cancel left pending, if there is one.
// A call can finish normally after should_cancel has asked to cancel it,
// and the KeyboardInterrupt must still reach the caller.
void raise_pending_error() {
    if (PyErr_Occurred()) {
        throw nb::python_error();
    }
}

// Run body with the GIL released, and return what it returns.
// body must not touch a Python object.
template <typename Body>
auto released(Body&& body) {
    using Result = decltype(body());
    if constexpr (std::is_void_v<Result>) {
        {
            nb::gil_scoped_release release;
            body();
        }
        raise_pending_error();
    } else {
        std::optional<Result> result;
        {
            nb::gil_scoped_release release;
            result.emplace(body());
        }
        raise_pending_error();
        return std::move(*result);
    }
}

// Polled by a waiting call. Returns true once a signal handler has raised,
// such as the KeyboardInterrupt that Ctrl-C raises,
// and leaves that exception pending for the caller.
// PyErr_CheckSignals runs the handlers only on the main thread,
// so a call on any other thread is never cancelled this way.
bool python_signal_pending() {
    nb::gil_scoped_acquire acquire;
    return PyErr_CheckSignals() != 0;
}

std::string task_get_response_repr(const ds::TaskGetResponse& response) {
    return nb::cast<std::string>(nb::str("TaskGetResponse(task_id={!r}, function={!r}, input={!r})")
                                     .format(response.task_id, to_bytes(response.function), to_bytes(response.input)));
}

} // namespace

NB_MODULE(_ext, m) {
    m.doc() = "The C++ ds-service client. Use ds_service_client, which wraps it.";

    nb::enum_<ds::TaskState>(m, "TaskState", nb::is_arithmetic(), "The state of a task.")
        .value("Waiting", ds::TaskState::Waiting)
        .value("Ready", ds::TaskState::Ready)
        .value("Running", ds::TaskState::Running)
        .value("Finished", ds::TaskState::Finished)
        .value("Failed", ds::TaskState::Failed)
        .value("Canceled", ds::TaskState::Canceled)
        .value("Undefined", ds::TaskState::Undefined);

    nb::enum_<ds::ErrorCode>(m, "ErrorCode", "Why a call failed.")
        .value("NotFound", ds::ErrorCode::NotFound)
        .value("AlreadyExists", ds::ErrorCode::AlreadyExists)
        .value("InvalidArgument", ds::ErrorCode::InvalidArgument)
        .value("FailedPrecondition", ds::ErrorCode::FailedPrecondition)
        .value("MessageTooLarge", ds::ErrorCode::MessageTooLarge)
        .value("Unavailable", ds::ErrorCode::Unavailable)
        .value("DeadlineExceeded", ds::ErrorCode::DeadlineExceeded)
        .value("Cancelled", ds::ErrorCode::Cancelled)
        .value("Closed", ds::ErrorCode::Closed)
        .value("Transport", ds::ErrorCode::Transport);

    // ClientError carries the ErrorCode as .code and the text as .message.
    // The translator registered here replaces the plain one nb::exception registers,
    // because nanobind tries the most recently registered translator first.
    static nb::exception<ds::ClientError> client_error(m, "ClientError");
    nb::register_exception_translator(
        [](const std::exception_ptr& pointer, void* payload) {
            try {
                std::rethrow_exception(pointer);
            } catch (const ds::ClientError& error) {
                // should_cancel left a KeyboardInterrupt pending, which is what the caller must see.
                if (PyErr_Occurred()) {
                    return;
                }
                nb::handle type{static_cast<PyObject*>(payload)};
                nb::object code = nb::cast(error.code());
                nb::str message{error.what()};
                nb::object instance = type(code, message);
                instance.attr("code") = code;
                instance.attr("message") = message;
                PyErr_SetObject(type.ptr(), instance.ptr());
            }
        },
        client_error.ptr());

    nb::class_<ds::TaskGetResponse>(m, "TaskGetResponse", "A task that task_get claimed.")
        .def_prop_ro("task_id", [](const ds::TaskGetResponse& self) { return self.task_id; })
        .def_prop_ro("function", [](const ds::TaskGetResponse& self) { return to_bytes(self.function); })
        .def_prop_ro("input", [](const ds::TaskGetResponse& self) { return to_bytes(self.input); })
        .def("__repr__", &task_get_response_repr)
        .def("__eq__", [](const ds::TaskGetResponse& self, const ds::TaskGetResponse& other) {
            return self.task_id == other.task_id && self.function == other.function && self.input == other.input;
        });

    nb::class_<ds::TaskGetCountByStateResponse>(m, "TaskGetCountByStateResponse", "How many tasks are in each state.")
        .def_ro("waiting", &ds::TaskGetCountByStateResponse::waiting)
        .def_ro("ready", &ds::TaskGetCountByStateResponse::ready)
        .def_ro("running", &ds::TaskGetCountByStateResponse::running)
        .def_ro("finished", &ds::TaskGetCountByStateResponse::finished)
        .def_ro("failed", &ds::TaskGetCountByStateResponse::failed)
        .def_ro("canceled", &ds::TaskGetCountByStateResponse::canceled)
        .def("__repr__",
             [](const ds::TaskGetCountByStateResponse& self) {
                 return nb::str("TaskGetCountByStateResponse(waiting={}, ready={}, running={}, finished={}, "
                                "failed={}, canceled={})")
                     .format(self.waiting, self.ready, self.running, self.finished, self.failed, self.canceled);
             })
        .def("__eq__", [](const ds::TaskGetCountByStateResponse& self, const ds::TaskGetCountByStateResponse& other) {
            return self.waiting == other.waiting && self.ready == other.ready && self.running == other.running &&
                   self.finished == other.finished && self.failed == other.failed && self.canceled == other.canceled;
        });

    nb::class_<ds::TimeSeriesDataPoint>(m, "TimeSeriesDataPoint", "One point of a time series.")
        .def_ro("value", &ds::TimeSeriesDataPoint::value)
        .def_ro("datetime", &ds::TimeSeriesDataPoint::datetime)
        .def_ro("step", &ds::TimeSeriesDataPoint::step)
        .def("__repr__",
             [](const ds::TimeSeriesDataPoint& self) {
                 return nb::str("TimeSeriesDataPoint(value={!r}, datetime={!r}, step={!r})")
                     .format(self.value, self.datetime, self.step);
             })
        .def("__eq__", [](const ds::TimeSeriesDataPoint& self, const ds::TimeSeriesDataPoint& other) {
            return self.value == other.value && self.datetime == other.datetime && self.step == other.step;
        });

    nb::class_<ds::Client>(m, "Client", "A connection to a ds-service server. Make one with connect().")
        .def(
            "map_set",
            [](ds::Client& self, std::string key, const nb::bytes& value) {
                released([&, value = from_bytes(value)]() mutable { self.map_set(std::move(key), std::move(value)); });
            },
            "key"_a, "value"_a)
        .def(
            "map_get",
            [](ds::Client& self, std::string key) {
                return to_bytes(released([&] { return self.map_get(std::move(key)); }));
            },
            "key"_a)
        .def(
            "map_search_key",
            [](ds::Client& self, std::string pattern) {
                return released([&] { return self.map_search_key(std::move(pattern)); });
            },
            "pattern"_a)

        .def(
            "task_add",
            [](ds::Client& self, std::string task_id, std::vector<std::string> parent_task_ids,
               std::vector<std::string> queue, double priority, const nb::bytes& function, const nb::bytes& input) {
                released([&, function = from_bytes(function), input = from_bytes(input)]() mutable {
                    self.task_add(std::move(task_id), std::move(parent_task_ids), std::move(queue), priority,
                                  std::move(function), std::move(input));
                });
            },
            "task_id"_a, "parent_task_ids"_a, "queue"_a, "priority"_a, "function"_a, "input"_a)
        .def(
            "task_get_status",
            [](ds::Client& self, std::vector<std::string> task_id) {
                return released([&] { return self.task_get_status(std::move(task_id)); });
            },
            "task_id"_a)
        .def(
            "task_get_output",
            [](ds::Client& self, std::string task_id) {
                return to_bytes(released([&] { return self.task_get_output(std::move(task_id)); }));
            },
            "task_id"_a)
        .def("task_get_count_by_state",
             [](ds::Client& self) { return released([&] { return self.task_get_count_by_state(); }); })
        .def(
            "task_cancel",
            [](ds::Client& self, std::string task_id) {
                return released([&] { return self.task_cancel(std::move(task_id)); });
            },
            "task_id"_a)
        .def(
            "task_get_priority",
            [](ds::Client& self, std::string task_id) {
                return released([&] { return self.task_get_priority(std::move(task_id)); });
            },
            "task_id"_a)
        .def(
            "task_set_priority",
            [](ds::Client& self, std::string task_id, double priority) {
                released([&] { self.task_set_priority(std::move(task_id), priority); });
            },
            "task_id"_a, "priority"_a)
        .def(
            "task_get_worker_id",
            [](ds::Client& self, std::string task_id) {
                return released([&] { return self.task_get_worker_id(std::move(task_id)); });
            },
            "task_id"_a)
        .def(
            "task_search_id",
            [](ds::Client& self, std::string pattern) {
                return released([&] { return self.task_search_id(std::move(pattern)); });
            },
            "pattern"_a)
        .def(
            "task_get",
            [](ds::Client& self, std::string worker_id, std::vector<std::string> queue) {
                return released([&] { return self.task_get(std::move(worker_id), std::move(queue)); });
            },
            "worker_id"_a, "queue"_a)
        .def(
            "task_done",
            [](ds::Client& self, std::string task_id, std::string worker_id, const nb::bytes& output, bool failed) {
                released([&, output = from_bytes(output)]() mutable {
                    self.task_done(std::move(task_id), std::move(worker_id), std::move(output), failed);
                });
            },
            "task_id"_a, "worker_id"_a, "output"_a, "failed"_a)

        .def(
            "journal_size",
            [](ds::Client& self, std::string key) {
                return released([&] { return self.journal_size(std::move(key)); });
            },
            "key"_a)
        .def(
            "journal_read",
            [](ds::Client& self, std::string key, std::uint64_t start, std::uint64_t end) {
                return to_bytes_list(released([&] { return self.journal_read(std::move(key), start, end); }));
            },
            "key"_a, "start"_a, "end"_a)
        .def(
            "journal_append",
            [](ds::Client& self, std::string key, const nb::bytes& value) {
                released([&, value = from_bytes(value)]() mutable {
                    self.journal_append(std::move(key), std::move(value));
                });
            },
            "key"_a, "value"_a)
        .def(
            "journal_search_key",
            [](ds::Client& self, std::string pattern) {
                return released([&] { return self.journal_search_key(std::move(pattern)); });
            },
            "pattern"_a)

        .def(
            "time_series_append",
            [](ds::Client& self, std::string key, double value, std::string datetime, std::int64_t step) {
                released([&] { self.time_series_append(std::move(key), value, std::move(datetime), step); });
            },
            "key"_a, "value"_a, "datetime"_a, "step"_a)
        .def(
            "time_series_get",
            [](ds::Client& self, std::string key, std::optional<std::string> start_time,
               std::optional<std::string> end_time, std::optional<std::int64_t> start_step,
               std::optional<std::int64_t> end_step) {
                return released([&] {
                    return self.time_series_get(std::move(key), std::move(start_time), std::move(end_time), start_step,
                                                end_step);
                });
            },
            "key"_a, "start_time"_a.none(), "end_time"_a.none(), "start_step"_a.none(), "end_step"_a.none())
        .def(
            "time_series_search_key",
            [](ds::Client& self, std::string pattern) {
                return released([&] { return self.time_series_search_key(std::move(pattern)); });
            },
            "pattern"_a)

        .def(
            "mutex_try_acquire",
            [](ds::Client& self, std::string key, std::string worker_id) {
                return released([&] { return self.mutex_try_acquire(std::move(key), std::move(worker_id)); });
            },
            "key"_a, "worker_id"_a)
        .def(
            "mutex_release",
            [](ds::Client& self, std::string key, std::string worker_id) {
                released([&] { self.mutex_release(std::move(key), std::move(worker_id)); });
            },
            "key"_a, "worker_id"_a)
        .def(
            "mutex_get_worker_id",
            [](ds::Client& self, std::string key) {
                return released([&] { return self.mutex_get_worker_id(std::move(key)); });
            },
            "key"_a)
        .def(
            "mutex_search_key",
            [](ds::Client& self, std::string pattern) {
                return released([&] { return self.mutex_search_key(std::move(pattern)); });
            },
            "pattern"_a)

        .def(
            "counter_get_next_value",
            [](ds::Client& self, std::string key) {
                return released([&] { return self.counter_get_next_value(std::move(key)); });
            },
            "key"_a)
        .def(
            "counter_get_current_value",
            [](ds::Client& self, std::string key) {
                return released([&] { return self.counter_get_current_value(std::move(key)); });
            },
            "key"_a)
        .def(
            "counter_search_key",
            [](ds::Client& self, std::string pattern) {
                return released([&] { return self.counter_search_key(std::move(pattern)); });
            },
            "pattern"_a)

        // close() does not block, so it keeps the GIL.
        .def("close", &ds::Client::close);

    m.def(
        "connect",
        [](const std::string& address, double timeout) {
            return ds::connect(address, {
                                            .timeout = std::chrono::duration<double>{timeout},
                                            .should_cancel = python_signal_pending,
                                        });
        },
        "address"_a, "timeout"_a,
        "Connect to the server at address, with timeout in seconds as the deadline of every call.");
}
