#include "codec.hpp"

#include <string>
#include <utility>
#include <vector>

namespace ds::grpc_codec {

namespace {

using RepeatedStrings = google::protobuf::RepeatedPtrField<std::string>;

// A reordering on either side, or a new value in the proto, fails the build here.
// Otherwise the two ends of the wire read one state as another.
static_assert(static_cast<int>(TaskState::Waiting) == ::TaskState::Waiting);
static_assert(static_cast<int>(TaskState::Ready) == ::TaskState::Ready);
static_assert(static_cast<int>(TaskState::Running) == ::TaskState::Running);
static_assert(static_cast<int>(TaskState::Finished) == ::TaskState::Finished);
static_assert(static_cast<int>(TaskState::Failed) == ::TaskState::Failed);
static_assert(static_cast<int>(TaskState::Canceled) == ::TaskState::Canceled);
static_assert(static_cast<int>(TaskState::Undefined) == ::TaskState::Undefined);
static_assert(::TaskState_MIN == ::TaskState::Waiting);
static_assert(::TaskState_MAX == ::TaskState::Undefined);

// Each code the server sends must come back as itself on the client,
// or a client reports a different failure than the server meant.
constexpr bool round_trips(ErrorCode code) {
    return from_status_code(to_status_code(code)) == code;
}
static_assert(round_trips(ErrorCode::NotFound));
static_assert(round_trips(ErrorCode::AlreadyExists));
static_assert(round_trips(ErrorCode::InvalidArgument));
static_assert(round_trips(ErrorCode::FailedPrecondition));
static_assert(round_trips(ErrorCode::MessageTooLarge));
static_assert(round_trips(ErrorCode::Unavailable));
static_assert(round_trips(ErrorCode::DeadlineExceeded));
static_assert(round_trips(ErrorCode::Cancelled));

std::vector<std::string> copy_strings(const RepeatedStrings& field) {
    return {field.begin(), field.end()};
}

std::vector<std::string> move_strings(RepeatedStrings* field) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(field->size()));
    for (auto& value : *field) {
        out.push_back(std::move(value));
    }
    return out;
}

void add_strings(std::vector<std::string>&& values, RepeatedStrings* out) {
    out->Reserve(static_cast<int>(values.size()));
    for (auto& value : values) {
        out->Add(std::move(value));
    }
}

} // namespace

::TaskState to_proto(TaskState state) {
    return static_cast<::TaskState>(state);
}

TaskState from_proto(int state) {
    if (state < ::TaskState_MIN || state > ::TaskState_MAX) {
        return TaskState::Undefined;
    }
    return static_cast<TaskState>(state);
}

grpc::Status to_status(const Error& error) {
    return grpc::Status(to_status_code(error.code), error.message);
}

// Server side: decode a request.

MapSetRequest decode(const ::MapSetRequest& message) {
    return {message.key(), message.value()};
}

MapGetRequest decode(const ::MapGetRequest& message) {
    return {message.key()};
}

SearchKeyRequest decode(const ::SearchKeyRequest& message) {
    return {message.pattern()};
}

TaskAddRequest decode(const ::TaskAddRequest& message) {
    return {
        .task_id = message.task_id(),
        .parent_task_ids = copy_strings(message.parent_task_ids()),
        .queue = copy_strings(message.queue()),
        .priority = message.priority(),
        .function = message.function(),
        .input = message.input(),
    };
}

TaskGetStatusRequest decode(const ::TaskGetStatusRequest& message) {
    return {copy_strings(message.task_id())};
}

TaskGetOutputRequest decode(const ::TaskGetOutputRequest& message) {
    return {message.task_id()};
}

TaskCancelRequest decode(const ::TaskCancelRequest& message) {
    return {message.task_id()};
}

TaskGetPriorityRequest decode(const ::TaskGetPriorityRequest& message) {
    return {message.task_id()};
}

TaskSetPriorityRequest decode(const ::TaskSetPriorityRequest& message) {
    return {message.task_id(), message.priority()};
}

TaskGetWorkerIdRequest decode(const ::TaskGetWorkerIdRequest& message) {
    return {message.task_id()};
}

TaskGetRequest decode(const ::TaskGetRequest& message) {
    return {message.worker_id(), copy_strings(message.queue())};
}

TaskDoneRequest decode(const ::TaskDoneRequest& message) {
    return {
        .task_id = message.task_id(),
        .output = message.output(),
        .worker_id = message.worker_id(),
        .failed = message.failed(),
    };
}

JournalSizeRequest decode(const ::JournalSizeRequest& message) {
    return {message.key()};
}

JournalReadRequest decode(const ::JournalReadRequest& message) {
    return {message.key(), message.start(), message.end()};
}

JournalAppendRequest decode(const ::JournalAppendRequest& message) {
    return {message.key(), message.value()};
}

TimeSeriesAppendRequest decode(const ::TimeSeriesAppendRequest& message) {
    return {message.key(), message.value(), message.datetime(), message.step()};
}

TimeSeriesGetRequest decode(const ::TimeSeriesGetRequest& message) {
    TimeSeriesGetRequest out;
    out.key = message.key();
    if (message.has_start_time()) {
        out.start_time = message.start_time();
    }
    if (message.has_end_time()) {
        out.end_time = message.end_time();
    }
    if (message.has_start_step()) {
        out.start_step = message.start_step();
    }
    if (message.has_end_step()) {
        out.end_step = message.end_step();
    }
    return out;
}

MutexTryAcquireRequest decode(const ::MutexTryAcquireRequest& message) {
    return {message.key(), message.worker_id()};
}

MutexReleaseRequest decode(const ::MutexReleaseRequest& message) {
    return {message.key(), message.worker_id()};
}

MutexGetWorkerIdRequest decode(const ::MutexGetWorkerIdRequest& message) {
    return {message.key()};
}

CounterGetNextValueRequest decode(const ::CounterGetNextValueRequest& message) {
    return {message.key()};
}

CounterGetCurrentValueRequest decode(const ::CounterGetCurrentValueRequest& message) {
    return {message.key()};
}

// Server side: encode a response.

void encode(MapGetResponse message, ::MapGetResponse* out) {
    out->set_value(std::move(message.value));
}

void encode(SearchKeyResponse message, ::SearchKeyResponse* out) {
    add_strings(std::move(message.key), out->mutable_key());
}

void encode(TaskGetStatusResponse message, ::TaskGetStatusResponse* out) {
    out->mutable_state()->Reserve(static_cast<int>(message.state.size()));
    for (const auto state : message.state) {
        out->add_state(to_proto(state));
    }
}

void encode(TaskGetOutputResponse message, ::TaskGetOutputResponse* out) {
    out->set_output(std::move(message.output));
}

void encode(TaskGetCountByStateResponse message, ::TaskGetCountByStateResponse* out) {
    out->set_waiting(message.waiting);
    out->set_ready(message.ready);
    out->set_running(message.running);
    out->set_finished(message.finished);
    out->set_failed(message.failed);
    out->set_canceled(message.canceled);
}

void encode(TaskCancelResponse message, ::TaskCancelResponse* out) {
    out->set_success(message.success);
}

void encode(TaskGetPriorityResponse message, ::TaskGetPriorityResponse* out) {
    out->set_priority(message.priority);
}

void encode(TaskGetWorkerIdResponse message, ::TaskGetWorkerIdResponse* out) {
    out->set_worker_id(std::move(message.worker_id));
}

void encode(TaskGetResponse message, ::TaskGetResponse* out) {
    out->set_task_id(std::move(message.task_id));
    out->set_function(std::move(message.function));
    out->set_input(std::move(message.input));
}

void encode(JournalSizeResponse message, ::JournalSizeResponse* out) {
    out->set_size(message.size);
}

void encode(JournalReadResponse message, ::JournalReadResponse* out) {
    add_strings(std::move(message.entry), out->mutable_entry());
}

void encode(TimeSeriesGetResponse message, ::TimeSeriesGetResponse* out) {
    out->mutable_point()->Reserve(static_cast<int>(message.point.size()));
    for (auto& point : message.point) {
        auto* added = out->add_point();
        added->set_value(point.value);
        added->set_datetime(std::move(point.datetime));
        added->set_step(point.step);
    }
}

void encode(MutexTryAcquireResponse message, ::MutexTryAcquireResponse* out) {
    out->set_acquired(message.acquired);
}

void encode(MutexGetWorkerIdResponse message, ::MutexGetWorkerIdResponse* out) {
    out->set_worker_id(std::move(message.worker_id));
}

void encode(CounterGetNextValueResponse message, ::CounterGetNextValueResponse* out) {
    out->set_value(message.value);
}

void encode(CounterGetCurrentValueResponse message, ::CounterGetCurrentValueResponse* out) {
    out->set_value(message.value);
}

// Client side: encode a request.
// Every field is set, including a field at its default,
// which is what the Python client has always sent.
// Only the optional bounds of TimeSeriesGetRequest are left unset when absent.

void encode(MapSetRequest message, ::MapSetRequest* out) {
    out->set_key(std::move(message.key));
    out->set_value(std::move(message.value));
}

void encode(MapGetRequest message, ::MapGetRequest* out) {
    out->set_key(std::move(message.key));
}

void encode(SearchKeyRequest message, ::SearchKeyRequest* out) {
    out->set_pattern(std::move(message.pattern));
}

void encode(TaskAddRequest message, ::TaskAddRequest* out) {
    out->set_task_id(std::move(message.task_id));
    add_strings(std::move(message.parent_task_ids), out->mutable_parent_task_ids());
    add_strings(std::move(message.queue), out->mutable_queue());
    out->set_priority(message.priority);
    out->set_function(std::move(message.function));
    out->set_input(std::move(message.input));
}

void encode(TaskGetStatusRequest message, ::TaskGetStatusRequest* out) {
    add_strings(std::move(message.task_id), out->mutable_task_id());
}

void encode(TaskGetOutputRequest message, ::TaskGetOutputRequest* out) {
    out->set_task_id(std::move(message.task_id));
}

void encode(TaskCancelRequest message, ::TaskCancelRequest* out) {
    out->set_task_id(std::move(message.task_id));
}

void encode(TaskGetPriorityRequest message, ::TaskGetPriorityRequest* out) {
    out->set_task_id(std::move(message.task_id));
}

void encode(TaskSetPriorityRequest message, ::TaskSetPriorityRequest* out) {
    out->set_task_id(std::move(message.task_id));
    out->set_priority(message.priority);
}

void encode(TaskGetWorkerIdRequest message, ::TaskGetWorkerIdRequest* out) {
    out->set_task_id(std::move(message.task_id));
}

void encode(TaskGetRequest message, ::TaskGetRequest* out) {
    out->set_worker_id(std::move(message.worker_id));
    add_strings(std::move(message.queue), out->mutable_queue());
}

void encode(TaskDoneRequest message, ::TaskDoneRequest* out) {
    out->set_task_id(std::move(message.task_id));
    out->set_output(std::move(message.output));
    out->set_worker_id(std::move(message.worker_id));
    out->set_failed(message.failed);
}

void encode(JournalSizeRequest message, ::JournalSizeRequest* out) {
    out->set_key(std::move(message.key));
}

void encode(JournalReadRequest message, ::JournalReadRequest* out) {
    out->set_key(std::move(message.key));
    out->set_start(message.start);
    out->set_end(message.end);
}

void encode(JournalAppendRequest message, ::JournalAppendRequest* out) {
    out->set_key(std::move(message.key));
    out->set_value(std::move(message.value));
}

void encode(TimeSeriesAppendRequest message, ::TimeSeriesAppendRequest* out) {
    out->set_key(std::move(message.key));
    out->set_value(message.value);
    out->set_datetime(std::move(message.datetime));
    out->set_step(message.step);
}

void encode(TimeSeriesGetRequest message, ::TimeSeriesGetRequest* out) {
    out->set_key(std::move(message.key));
    if (message.start_time) {
        out->set_start_time(std::move(*message.start_time));
    }
    if (message.end_time) {
        out->set_end_time(std::move(*message.end_time));
    }
    if (message.start_step) {
        out->set_start_step(*message.start_step);
    }
    if (message.end_step) {
        out->set_end_step(*message.end_step);
    }
}

void encode(MutexTryAcquireRequest message, ::MutexTryAcquireRequest* out) {
    out->set_key(std::move(message.key));
    out->set_worker_id(std::move(message.worker_id));
}

void encode(MutexReleaseRequest message, ::MutexReleaseRequest* out) {
    out->set_key(std::move(message.key));
    out->set_worker_id(std::move(message.worker_id));
}

void encode(MutexGetWorkerIdRequest message, ::MutexGetWorkerIdRequest* out) {
    out->set_key(std::move(message.key));
}

void encode(CounterGetNextValueRequest message, ::CounterGetNextValueRequest* out) {
    out->set_key(std::move(message.key));
}

void encode(CounterGetCurrentValueRequest message, ::CounterGetCurrentValueRequest* out) {
    out->set_key(std::move(message.key));
}

// Client side: decode a response.

MapGetResponse decode(::MapGetResponse&& message) {
    return {std::move(*message.mutable_value())};
}

SearchKeyResponse decode(::SearchKeyResponse&& message) {
    return {move_strings(message.mutable_key())};
}

TaskGetStatusResponse decode(::TaskGetStatusResponse&& message) {
    TaskGetStatusResponse out;
    out.state.reserve(static_cast<std::size_t>(message.state_size()));
    for (const auto state : message.state()) {
        out.state.push_back(from_proto(state));
    }
    return out;
}

TaskGetOutputResponse decode(::TaskGetOutputResponse&& message) {
    return {std::move(*message.mutable_output())};
}

TaskGetCountByStateResponse decode(::TaskGetCountByStateResponse&& message) {
    return {
        .waiting = message.waiting(),
        .ready = message.ready(),
        .running = message.running(),
        .finished = message.finished(),
        .failed = message.failed(),
        .canceled = message.canceled(),
    };
}

TaskCancelResponse decode(::TaskCancelResponse&& message) {
    return {message.success()};
}

TaskGetPriorityResponse decode(::TaskGetPriorityResponse&& message) {
    return {message.priority()};
}

TaskGetWorkerIdResponse decode(::TaskGetWorkerIdResponse&& message) {
    return {std::move(*message.mutable_worker_id())};
}

TaskGetResponse decode(::TaskGetResponse&& message) {
    return {
        .task_id = std::move(*message.mutable_task_id()),
        .function = std::move(*message.mutable_function()),
        .input = std::move(*message.mutable_input()),
    };
}

JournalSizeResponse decode(::JournalSizeResponse&& message) {
    return {message.size()};
}

JournalReadResponse decode(::JournalReadResponse&& message) {
    return {move_strings(message.mutable_entry())};
}

TimeSeriesGetResponse decode(::TimeSeriesGetResponse&& message) {
    TimeSeriesGetResponse out;
    out.point.reserve(static_cast<std::size_t>(message.point_size()));
    for (auto& point : *message.mutable_point()) {
        out.point.push_back({point.value(), std::move(*point.mutable_datetime()), point.step()});
    }
    return out;
}

MutexTryAcquireResponse decode(::MutexTryAcquireResponse&& message) {
    return {message.acquired()};
}

MutexGetWorkerIdResponse decode(::MutexGetWorkerIdResponse&& message) {
    return {std::move(*message.mutable_worker_id())};
}

CounterGetNextValueResponse decode(::CounterGetNextValueResponse&& message) {
    return {message.value()};
}

CounterGetCurrentValueResponse decode(::CounterGetCurrentValueResponse&& message) {
    return {message.value()};
}

} // namespace ds::grpc_codec
