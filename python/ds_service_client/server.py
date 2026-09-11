"""Temporary ds-service server processes.

Starts a ds-service process for the lifetime of a `DsServiceServer` object.
"""

import os
import shlex
import signal
import socket
import subprocess
import time
from types import TracebackType

import ifaddr

# Environment variable consulted when no ds_service_bin is passed.
DS_SERVICE_BIN_ENV_VAR = "DS_SERVICE_BIN"

# Used when neither the argument nor the environment variable is set,
# that is, a ds-service on the PATH.
DEFAULT_DS_SERVICE_BIN = "ds-service"

# How long close() waits for a server to exit after SIGTERM, before SIGKILL.
TERMINATE_TIMEOUT_S = 10.0

# Gap between connection attempts in wait_until_ready.
READY_POLL_INTERVAL_S = 0.01

# Gap between checks that the server's process group has gone, in close().
EXIT_POLL_INTERVAL_S = 0.01


def resolve_ds_service_bin(ds_service_bin: str | None = None) -> str:
    """How to start the server: the argument, $DS_SERVICE_BIN, or the default.

    A blank value counts as unset.
    An exported but empty DS_SERVICE_BIN is how a shell says "no value".
    If the command takes it literally, it starts at `--address`,
    which fails as a missing-executable error naming a flag.
    """
    for candidate in (ds_service_bin, os.environ.get(DS_SERVICE_BIN_ENV_VAR)):
        if candidate and candidate.strip():
            return candidate.strip()

    return DEFAULT_DS_SERVICE_BIN


def resolve_interface_ipv4(interface: str) -> str:
    """The IPv4 address assigned to interface, as a dotted quad.

    Raises ValueError if this machine has no such interface,
    or has it but with no IPv4 address on it.
    Neither case has an address to bind.
    A bind to some other address puts the server on a network
    the caller did not ask for.
    """
    known = []
    for adapter in ifaddr.get_adapters():
        known.append(adapter.name)
        if adapter.name != interface and adapter.nice_name != interface:
            continue

        for adapter_ip in adapter.ips:
            if adapter_ip.is_IPv4:
                return str(adapter_ip.ip)

        raise ValueError(
            f"Interface {interface!r} has no IPv4 address assigned; "
            f"ds-service is reachable over IPv4 only."
        )

    raise ValueError(
        f"No interface named {interface!r} on this machine; "
        f"the interfaces here are {', '.join(sorted(known))}."
    )


def _free_port(host: str) -> int:
    """Reserve an ephemeral IPv4 port on host and return it."""
    # This function closes the socket before the server starts.
    # Nothing holds the port after that,
    # and the kernel is only unlikely to hand it out again immediately.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return sock.getsockname()[1]


def _check_port_free(host: str, port: int) -> None:
    """Raise OSError if something already holds the port."""
    # A server started on an occupied port loses the race and exits,
    # while the port keeps accepting connections.
    # Without this check,
    # the constructor hands the caller a dead DsServiceServer
    # whose address belongs to somebody else's server.
    # The caller then reads and writes that server's state as its own.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        # This probe sets SO_REUSEADDR because gRPC's own listener sets it.
        # Without it, the probe also fails on a port left in TIME_WAIT
        # by a server that already exited.
        # The real server can bind such a port.
        # The probe still fails against a live listener,
        # which is what it is here for.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((host, port))
        except OSError as exc:
            raise OSError(
                f"Cannot start ds-service on {host}:{port}: "
                f"the port is already in use ({exc})."
            ) from exc


class DsServiceServer:
    """A ds-service process that runs for as long as this object does.

    The constructor starts the process,
    so the server is already starting when it returns.
    Call wait_until_ready() before connecting,
    and close() to stop it.
    """

    def __init__(
        self,
        interface: str,
        port: int | None = None,
        ds_service_bin: str | None = None,
    ) -> None:
        """Start a ds-service process bound to the interface's IPv4 address.

        port defaults to a free ephemeral port, and 0 means the same.
        ds_service_bin can be a whole command rather than a path.
        It defaults to $DS_SERVICE_BIN, then to a ds-service on PATH.
        Raises ValueError if the interface is unknown
        or has no IPv4 address,
        and OSError if an explicitly given port is already in use.
        The constructor starts nothing when it raises either error.
        """
        host = resolve_interface_ipv4(interface)

        ds_service_bin = resolve_ds_service_bin(ds_service_bin)

        # Port 0 asks the kernel for an ephemeral port,
        # so it means the same here as passing port = None.
        if not port:
            port = _free_port(host)
        else:
            _check_port_free(host, port)

        self.interface = interface
        self.host = host
        self.port = port
        self.ds_service_bin = ds_service_bin
        self.address = f"{host}:{port}"

        self.command = f"{ds_service_bin} --address {self.address}"

        # start_new_session puts the server in its own process group,
        # so close() can signal the whole group.
        # A container runtime or a wrapper script can leave children behind.
        # A signal to the started process alone orphans the server.
        self.process = subprocess.Popen(
            shlex.split(self.command), start_new_session=True
        )

        # start_new_session makes the child a session and group leader,
        # so its pid is the group id.
        # The pid is kept because os.getpgid() stops
        # after the process is reaped,
        # and close() can run after that.
        self.pgid = self.process.pid

        # Guards close() against running twice. See its docstring.
        self._closed = False

    def wait_until_ready(self, timeout: int = 30) -> None:
        """Block until the server accepts TCP connections.

        Raises TimeoutError if it is not listening within timeout seconds,
        and RuntimeError if the process exits before then.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._raise_if_exited()

            try:
                with socket.create_connection((self.host, self.port), timeout=timeout):
                    pass
            except OSError:
                time.sleep(READY_POLL_INTERVAL_S)
                continue

            # Something is listening.
            # Check that it is still this server.
            # A process that exited by now lost the port to another server.
            # A return then hands the caller that one.
            self._raise_if_exited()
            return

        raise TimeoutError(
            f"ds-service did not start listening on {self.address} "
            f"within {timeout}s: {self.command}"
        )

    def _raise_if_exited(self) -> None:
        """Raise RuntimeError if the server process is no longer running."""
        returncode = self.process.poll()
        if returncode is not None:
            raise RuntimeError(
                f"ds-service exited with code {returncode} "
                f"before listening on {self.address}: {self.command}"
            )

    def close(self) -> None:
        """Stop the server: SIGTERM, then SIGKILL if it did not exit.

        This method signals the whole process group,
        not just the process the constructor started.
        A group signal also stops a server left behind
        by a wrapper that exited earlier.

        Safe to call more than once: the second call does nothing.
        """
        if self._closed:
            return

        # close() sets the flag before the teardown, not after.
        # A failure partway through then still bars a second run,
        # so nothing signals the group again.
        self._closed = True

        deadline = time.monotonic() + TERMINATE_TIMEOUT_S

        self._signal_process_group(signal.SIGTERM)

        if self.process.poll() is None:
            try:
                self.process.wait(timeout=max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass

        while self._process_group_alive() and time.monotonic() < deadline:
            time.sleep(EXIT_POLL_INTERVAL_S)

        if self._process_group_alive():
            self._signal_process_group(signal.SIGKILL)

        if self.process.poll() is None:
            self.process.wait()

    def _signal_process_group(self, signal_number: int) -> None:
        """Signal the server's process group.

        Do nothing if the group already exited.
        """
        try:
            os.killpg(self.pgid, signal_number)
        except ProcessLookupError:
            pass

    def _process_group_alive(self) -> bool:
        """Whether any process is left in the server's process group.

        Signal 0 checks for the group without signaling it.
        """
        try:
            os.killpg(self.pgid, 0)
            return True
        except ProcessLookupError:
            return False

    def __enter__(self) -> "DsServiceServer":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()
