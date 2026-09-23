#pragma once

#include <expected>
#include <stdexcept>
#include <string>
#include <utility>

namespace ds {

// Why an operation failed.
enum class ErrorCode {
    // Sent by the server.
    NotFound,
    AlreadyExists,
    InvalidArgument,
    FailedPrecondition,
    // Raised on the client side by a transport.
    MessageTooLarge,
    Unavailable,
    // Also what Client::mutex_acquire throws once its timeout has elapsed.
    DeadlineExceeded,
    Cancelled,
    // The client was closed before or during the call.
    Closed,
    // A transport failure that no other code describes.
    Transport,
};

// A failed operation: its code, and a message for a person to read.
struct Error {
    ErrorCode code;
    std::string message;
};

// What an operation of the server core returns.
// See "The transport boundary" in docs/developer-notes.md for why it is not an exception.
template <typename T>
using Result = std::expected<T, Error>;

// The failed Result of an operation, ready to return.
inline std::unexpected<Error> make_error(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

// What the C++ client throws when an operation fails.
class ClientError : public std::runtime_error {
  public:
    ClientError(ErrorCode code, const std::string& message)
        : std::runtime_error(message)
        , code_(code) {}

    ErrorCode code() const noexcept {
        return code_;
    }

  private:
    ErrorCode code_;
};

} // namespace ds
