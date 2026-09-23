#pragma once

// Conversions between the plain messages in ds-service/messages.hpp
// and the protobuf messages generated from cpp/grpc/ds-service.proto.
//
// This file is the only place that converts between the two.
// The server transport uses one half of it,
// and a client transport uses the other half:
//
// - The server decodes requests and encodes responses.
//   A request arrives const, so decoding it copies each field.
//   An encoded response moves each field out of the plain message.
// - A client encodes requests and decodes responses.
//   An encoded request moves each field out of the plain message.
//   A client owns the response it receives,
//   so decoding it moves each field out of the protobuf message.
//
// The proto's messages are in the global namespace,
// so this file names them with a leading ::.

#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>

#include "ds-service/error.hpp"
#include "ds-service/messages.hpp"
#include "ds-service/task-state.hpp"

namespace ds::grpc_codec {

// The proto value of state.
// cpp/grpc/codec.cpp checks that the two enums agree.
::TaskState to_proto(TaskState state);
// The plain value of a proto state.
// A value this build does not know, from a newer peer, reads as Undefined.
TaskState from_proto(int state);

// The gRPC status code for each ErrorCode.
// Closed and Transport have no counterpart, and map to UNKNOWN.
constexpr grpc::StatusCode to_status_code(ErrorCode code) {
    switch (code) {
    case ErrorCode::NotFound:
        return grpc::StatusCode::NOT_FOUND;
    case ErrorCode::AlreadyExists:
        return grpc::StatusCode::ALREADY_EXISTS;
    case ErrorCode::InvalidArgument:
        return grpc::StatusCode::INVALID_ARGUMENT;
    case ErrorCode::FailedPrecondition:
        return grpc::StatusCode::FAILED_PRECONDITION;
    case ErrorCode::MessageTooLarge:
        return grpc::StatusCode::RESOURCE_EXHAUSTED;
    case ErrorCode::Unavailable:
        return grpc::StatusCode::UNAVAILABLE;
    case ErrorCode::DeadlineExceeded:
        return grpc::StatusCode::DEADLINE_EXCEEDED;
    case ErrorCode::Cancelled:
        return grpc::StatusCode::CANCELLED;
    case ErrorCode::Closed:
    case ErrorCode::Transport:
        break;
    }
    return grpc::StatusCode::UNKNOWN;
}

// The ErrorCode for a gRPC status code other than OK.
// A code with no counterpart is Transport.
constexpr ErrorCode from_status_code(grpc::StatusCode code) {
    switch (code) {
    case grpc::StatusCode::NOT_FOUND:
        return ErrorCode::NotFound;
    case grpc::StatusCode::ALREADY_EXISTS:
        return ErrorCode::AlreadyExists;
    case grpc::StatusCode::INVALID_ARGUMENT:
        return ErrorCode::InvalidArgument;
    case grpc::StatusCode::FAILED_PRECONDITION:
        return ErrorCode::FailedPrecondition;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
        return ErrorCode::MessageTooLarge;
    case grpc::StatusCode::UNAVAILABLE:
        return ErrorCode::Unavailable;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
        return ErrorCode::DeadlineExceeded;
    case grpc::StatusCode::CANCELLED:
        return ErrorCode::Cancelled;
    default:
        return ErrorCode::Transport;
    }
}

// The gRPC status for error, with the code to_status_code gives it and its message.
grpc::Status to_status(const Error& error);

// Server side: decode a request.
MapSetRequest decode(const ::MapSetRequest& message);
MapGetRequest decode(const ::MapGetRequest& message);
SearchKeyRequest decode(const ::SearchKeyRequest& message);
TaskAddRequest decode(const ::TaskAddRequest& message);
TaskGetStatusRequest decode(const ::TaskGetStatusRequest& message);
TaskGetOutputRequest decode(const ::TaskGetOutputRequest& message);
TaskCancelRequest decode(const ::TaskCancelRequest& message);
TaskGetPriorityRequest decode(const ::TaskGetPriorityRequest& message);
TaskSetPriorityRequest decode(const ::TaskSetPriorityRequest& message);
TaskGetWorkerIdRequest decode(const ::TaskGetWorkerIdRequest& message);
TaskGetRequest decode(const ::TaskGetRequest& message);
TaskDoneRequest decode(const ::TaskDoneRequest& message);
JournalSizeRequest decode(const ::JournalSizeRequest& message);
JournalReadRequest decode(const ::JournalReadRequest& message);
JournalAppendRequest decode(const ::JournalAppendRequest& message);
TimeSeriesAppendRequest decode(const ::TimeSeriesAppendRequest& message);
TimeSeriesGetRequest decode(const ::TimeSeriesGetRequest& message);
MutexTryAcquireRequest decode(const ::MutexTryAcquireRequest& message);
MutexReleaseRequest decode(const ::MutexReleaseRequest& message);
MutexGetWorkerIdRequest decode(const ::MutexGetWorkerIdRequest& message);
CounterGetNextValueRequest decode(const ::CounterGetNextValueRequest& message);
CounterGetCurrentValueRequest decode(const ::CounterGetCurrentValueRequest& message);

// Server side: encode a response.
void encode(MapGetResponse message, ::MapGetResponse* out);
void encode(SearchKeyResponse message, ::SearchKeyResponse* out);
void encode(TaskGetStatusResponse message, ::TaskGetStatusResponse* out);
void encode(TaskGetOutputResponse message, ::TaskGetOutputResponse* out);
void encode(TaskGetCountByStateResponse message, ::TaskGetCountByStateResponse* out);
void encode(TaskCancelResponse message, ::TaskCancelResponse* out);
void encode(TaskGetPriorityResponse message, ::TaskGetPriorityResponse* out);
void encode(TaskGetWorkerIdResponse message, ::TaskGetWorkerIdResponse* out);
void encode(TaskGetResponse message, ::TaskGetResponse* out);
void encode(JournalSizeResponse message, ::JournalSizeResponse* out);
void encode(JournalReadResponse message, ::JournalReadResponse* out);
void encode(TimeSeriesGetResponse message, ::TimeSeriesGetResponse* out);
void encode(MutexTryAcquireResponse message, ::MutexTryAcquireResponse* out);
void encode(MutexGetWorkerIdResponse message, ::MutexGetWorkerIdResponse* out);
void encode(CounterGetNextValueResponse message, ::CounterGetNextValueResponse* out);
void encode(CounterGetCurrentValueResponse message, ::CounterGetCurrentValueResponse* out);

// Client side: encode a request.
void encode(MapSetRequest message, ::MapSetRequest* out);
void encode(MapGetRequest message, ::MapGetRequest* out);
void encode(SearchKeyRequest message, ::SearchKeyRequest* out);
void encode(TaskAddRequest message, ::TaskAddRequest* out);
void encode(TaskGetStatusRequest message, ::TaskGetStatusRequest* out);
void encode(TaskGetOutputRequest message, ::TaskGetOutputRequest* out);
void encode(TaskCancelRequest message, ::TaskCancelRequest* out);
void encode(TaskGetPriorityRequest message, ::TaskGetPriorityRequest* out);
void encode(TaskSetPriorityRequest message, ::TaskSetPriorityRequest* out);
void encode(TaskGetWorkerIdRequest message, ::TaskGetWorkerIdRequest* out);
void encode(TaskGetRequest message, ::TaskGetRequest* out);
void encode(TaskDoneRequest message, ::TaskDoneRequest* out);
void encode(JournalSizeRequest message, ::JournalSizeRequest* out);
void encode(JournalReadRequest message, ::JournalReadRequest* out);
void encode(JournalAppendRequest message, ::JournalAppendRequest* out);
void encode(TimeSeriesAppendRequest message, ::TimeSeriesAppendRequest* out);
void encode(TimeSeriesGetRequest message, ::TimeSeriesGetRequest* out);
void encode(MutexTryAcquireRequest message, ::MutexTryAcquireRequest* out);
void encode(MutexReleaseRequest message, ::MutexReleaseRequest* out);
void encode(MutexGetWorkerIdRequest message, ::MutexGetWorkerIdRequest* out);
void encode(CounterGetNextValueRequest message, ::CounterGetNextValueRequest* out);
void encode(CounterGetCurrentValueRequest message, ::CounterGetCurrentValueRequest* out);

// Client side: decode a response.
MapGetResponse decode(::MapGetResponse&& message);
SearchKeyResponse decode(::SearchKeyResponse&& message);
TaskGetStatusResponse decode(::TaskGetStatusResponse&& message);
TaskGetOutputResponse decode(::TaskGetOutputResponse&& message);
TaskGetCountByStateResponse decode(::TaskGetCountByStateResponse&& message);
TaskCancelResponse decode(::TaskCancelResponse&& message);
TaskGetPriorityResponse decode(::TaskGetPriorityResponse&& message);
TaskGetWorkerIdResponse decode(::TaskGetWorkerIdResponse&& message);
TaskGetResponse decode(::TaskGetResponse&& message);
JournalSizeResponse decode(::JournalSizeResponse&& message);
JournalReadResponse decode(::JournalReadResponse&& message);
TimeSeriesGetResponse decode(::TimeSeriesGetResponse&& message);
MutexTryAcquireResponse decode(::MutexTryAcquireResponse&& message);
MutexGetWorkerIdResponse decode(::MutexGetWorkerIdResponse&& message);
CounterGetNextValueResponse decode(::CounterGetNextValueResponse&& message);
CounterGetCurrentValueResponse decode(::CounterGetCurrentValueResponse&& message);

} // namespace ds::grpc_codec
