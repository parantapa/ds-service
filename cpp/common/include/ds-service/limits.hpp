#pragma once

#include <cstddef>

// CMakeLists.txt defines this on ds-service-common
// from the cache variable of the same name.
#ifndef DS_SERVICE_MAX_MESSAGE_SIZE
#error "DS_SERVICE_MAX_MESSAGE_SIZE is not defined. Link the ds-service-common target."
#endif

namespace ds {

// The largest request or response that any transport accepts, in bytes.
// It is set when the build is configured,
// with -DDS_SERVICE_MAX_MESSAGE_SIZE, and is 32 MiB by default.
// The server and its clients must be built with the same value.
inline constexpr std::size_t MAX_MESSAGE_SIZE_BYTES = DS_SERVICE_MAX_MESSAGE_SIZE;

static_assert(MAX_MESSAGE_SIZE_BYTES > 0, "DS_SERVICE_MAX_MESSAGE_SIZE must be positive");

} // namespace ds
