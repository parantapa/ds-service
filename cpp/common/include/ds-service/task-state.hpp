#pragma once

namespace ds {

// The state of a task.
// The names and values match the TaskState enum in cpp/grpc/ds-service.proto,
// and cpp/grpc/codec.cpp checks that they still do.
//
// Waiting is a task held back by a parent that has not finished.
// Finished is a task that ran without error.
// Failed is a task that ran and reported an error,
// or one that never ran because a task it depends on failed.
// Canceled is a task that TaskCancel canceled,
// or one that never ran because a task it depends on was canceled.
// TaskGetStatus reports Undefined for a task_id that does not exist.
enum class TaskState : int {
    Waiting = 0,
    Ready = 1,
    Running = 2,
    Finished = 3,
    Failed = 4,
    Canceled = 5,
    Undefined = 6,
};

} // namespace ds
