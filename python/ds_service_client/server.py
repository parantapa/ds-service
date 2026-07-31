"""Temporary ds-service server processes.

Starts a ds-service process for the lifetime of a `DsServiceServer` object.
"""

import os
import shlex
import signal
import socket
import subprocess
import time
import warnings

import ifaddr

# Environment variable consulted when no ds_service_bin is passed.
DS_SERVICE_BIN_ENV_VAR = "DS_SERVICE_BIN"

# Used when neither the argument nor the environment variable is set,
# i.e. a ds-service on the PATH.
DEFAULT_DS_SERVICE_BIN = "ds-service"

# Address used to talk to the server
# when it listens on the wildcard address,
# and when an interface lookup fails.
LOOPBACK_IP = "127.0.0.1"

# Bound on every interface, so not connectable as-is.
WILDCARD_IP = "0.0.0.0"

# How long close() waits for a SIGTERM'd server to exit before SIGKILL.
TERMINATE_TIMEOUT_S = 10.0

# Gap between connection attempts in wait_until_ready.
READY_POLL_INTERVAL_S = 0.01

# Gap between checks that the server's process group has gone, in close().
EXIT_POLL_INTERVAL_S = 0.01


def resolve_ds_service_bin(ds_service_bin: str | None = None) -> str:
    """How to start the server: the argument, $DS_SERVICE_BIN, or the default.

    A blank value counts as unset.
    An exported but empty DS_SERVICE_BIN is how a shell says "no value",
    and taking it literally
    would leave the command starting at `--address`,
    which fails as a missing-executable error naming a flag.
    """
    for candidate in (ds_service_bin, os.environ.get(DS_SERVICE_BIN_ENV_VAR)):
        if candidate and candidate.strip():
            return candidate.strip()

    return DEFAULT_DS_SERVICE_BIN


def _free_port(host: str) -> int:
    """Reserve an ephemeral IPv4 port on host and return it.

    The socket is closed before the server is started,
    so the port is only reserved in the sense that
    the kernel is unlikely to hand it out again immediately.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return sock.getsockname()[1]


def _check_port_free(host: str, port: int) -> None:
    """Raise if something already holds the port, before starting a server.

    A server started on an occupied port loses the race and exits,
    while the port keeps accepting connections --
    so without this check
    the caller would be handed a dead DsServiceServer
    whose address belongs to somebody else's server,
    and would read and write that server's state believing it is theirs.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        try:
            sock.bind((host, port))
        except OSError as exc:
            raise OSError(
                f"Cannot start ds-service on {host}:{port}: "
                f"the port is already in use ({exc})."
            ) from exc


class DsServiceServer:
    """A ds-service process that runs for as long as this object does.

    The process is started by the constructor,
    so the server is already coming up when it returns;
    use wait_until_ready() before connecting,
    and close() to stop it.

    ds_service_bin (or DS_SERVICE_BIN) may be a full command line
    rather than a path
    -- `apptainer run ds-service.sif` or
    `docker run --rm ... ds-service` work as well as
    `/usr/bin/ds-service`.
    `--address <host>:<port>` is appended to whatever is given,
    and the result is split with shlex, i.e. quoting is understood
    but shell syntax -- a pipeline, a redirection, a `FOO=bar` prefix
    -- is not.
    Wrap such a command in a script of your own if you need one.

    IPv4 only:
    host must be a dotted-quad address or a name that resolves to one,
    since the address is passed to the server as a plain `host:port` string,
    which has no way to spell an IPv6 address.
    """

    def __init__(
        self,
        host: str = WILDCARD_IP,
        port: int | None = None,
        ds_service_bin: str | None = None,
    ):
        # An IPv6 literal would make a `host:port` string ambiguous
        # ("::1:5051"),
        # so reject it here
        # rather than letting it fail deeper down
        # as an unrelated-looking socket error.
        if ":" in host:
            raise ValueError(
                f"IPv6 is not supported, and {host!r} looks like an IPv6 "
                f"address; give an IPv4 address such as {WILDCARD_IP}."
            )

        ds_service_bin = resolve_ds_service_bin(ds_service_bin)

        # Port 0 is how the kernel is asked for an ephemeral port,
        # so it means the same here as passing port = None.
        if not port:
            port = _free_port(host)
        else:
            _check_port_free(host, port)

        self.host = host
        self.port = port
        self.ds_service_bin = ds_service_bin
        self.address = f"{host}:{port}"

        self.command = f"{ds_service_bin} --address {self.address}"

        # start_new_session puts the server in its own process group,
        # so close() can signal the whole group.
        # A container runtime or a wrapper script may leave children behind,
        # and signalling only the process we started
        # would orphan the server.
        self.process = subprocess.Popen(
            shlex.split(self.command), start_new_session=True
        )

        # start_new_session makes the child a session and group leader,
        # so its pid is the group id.
        # Kept because os.getpgid() stops working
        # once the process is reaped,
        # and close() may run after that.
        self.pgid = self.process.pid

    @property
    def connect_host(self) -> str:
        """Host to connect to, for a server listening on the wildcard address."""
        if self.host == WILDCARD_IP:
            return LOOPBACK_IP
        return self.host

    def wait_until_ready(self, timeout: int = 30) -> None:
        """Block until the server accepts TCP connections.

        Raises TimeoutError if it is not listening within timeout seconds,
        and RuntimeError if the process exits before then.
        """
        host = self.connect_host
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._raise_if_exited()

            try:
                with socket.create_connection((host, self.port), timeout=timeout):
                    pass
            except OSError:
                time.sleep(READY_POLL_INTERVAL_S)
                continue

            # Something is listening -- check it is still us.
            # A process that has exited by now
            # lost the port to another server,
            # and returning would hand the caller that one.
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

    def get_address_by_interface(self, interface: str) -> str:
        """Return `<ip of interface>:<port>`, for handing to remote clients.

        Falls back to 127.0.0.1 with a warning
        if the interface does not exist on this machine
        or has no IPv4 address.
        """
        ip = None
        for adapter in ifaddr.get_adapters():
            if adapter.name == interface or adapter.nice_name == interface:
                for adapter_ip in adapter.ips:
                    if adapter_ip.is_IPv4:
                        ip = adapter_ip.ip
                        break
                break

        if ip is None:
            warnings.warn(
                f"No IPv4 address found for interface {interface!r}; "
                f"falling back to {LOOPBACK_IP}.",
                stacklevel=2,
            )
            ip = LOOPBACK_IP

        return f"{ip}:{self.port}"

    def close(self) -> None:
        """Stop the server: SIGTERM, then SIGKILL if it has not exited.

        The whole process group is signalled,
        not just the process that was started,
        so a server left behind by a wrapper that has since exited
        is stopped too.

        Safe to call more than once.
        """
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
        """Signal the server's process group, ignoring one that has exited."""
        try:
            os.killpg(self.pgid, signal_number)
        except ProcessLookupError:
            pass

    def _process_group_alive(self) -> bool:
        """Whether any process is left in the server's process group.

        Signal 0 checks for the group without signalling it.
        """
        try:
            os.killpg(self.pgid, 0)
            return True
        except ProcessLookupError:
            return False

    def __enter__(self) -> "DsServiceServer":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()
