#pragma once

#include <atomic>

#include "core/data-structures.hpp"

// Every top level data structure the server holds.
// main owns the one instance,
// and each transport reaches it through the reference start() receives.
struct SystemState {
    Map map{};

    JournalMap journal_map{};

    TimeSeriesMap time_series{};

    Mutexes mutexes{};

    Counters counters{};

    TaskManager task_manager{};

    // Written by the shutdown thread, so not a plain bool.
    std::atomic<bool> shutdown{false};
};
