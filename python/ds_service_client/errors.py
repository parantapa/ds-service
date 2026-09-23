"""The exceptions the ds-service clients raise, beyond the built-in ones."""


class NoTaskAvailable(Exception):
    """Raised by task_get when no queue it polled has work ready."""


class TaskStateError(RuntimeError):
    """Raised when an operation does not match a task's current state."""


class MutexNotHeld(RuntimeError):
    """Raised when an operation needs a mutex that is free or held by another."""


class TransportError(Exception):
    """Raised when a call fails for a reason the client does not map to another exception.

    The message is the transport's own.
    The exception chains from the error the extension module raised.
    """
