"""Tests for the client in a process created by fork().

The gRPC inside the extension module does not survive fork().
The client refuses calls in a child forked after a client existed,
rather than hang there.
"""

import subprocess
import sys
from pathlib import Path

PYTHON_DIR = Path(__file__).resolve().parents[1] / "python"

# The child reports its outcome through its exit status:
# 0 for the expected outcome, 1 for anything else.
# The parent waits at most CHILD_TIMEOUT_S, so a hang fails the test.
SCRIPT = """
import os, sys, time
from ds_service_client import DsServiceClient

address, scenario = sys.argv[1], sys.argv[2]
CHILD_TIMEOUT_S = 10.0

if scenario == "client-before-fork":
    parent = DsServiceClient(address, timeout=5.0)
    parent.map_set("k", b"v")

pid = os.fork()
if pid == 0:
    try:
        if scenario == "client-before-fork":
            for call in (lambda: parent.map_get("k"), lambda: DsServiceClient(address)):
                try:
                    call()
                    os._exit(1)
                except RuntimeError as e:
                    if "fork" not in str(e):
                        os._exit(1)
        else:
            if DsServiceClient(address, timeout=5.0).counter_get_next_value("n") != 1:
                os._exit(1)
        os._exit(0)
    except BaseException:
        os._exit(1)

deadline = time.monotonic() + CHILD_TIMEOUT_S
while time.monotonic() < deadline:
    done, status = os.waitpid(pid, os.WNOHANG)
    if done:
        sys.exit(os.waitstatus_to_exitcode(status))
    time.sleep(0.05)
os.kill(pid, 9)
sys.exit("the child hung")
"""


# Each test runs its scenario in a fresh interpreter,
# because this process has created clients already.
# The script enforces CHILD_TIMEOUT_S itself,
# so the outer timeout is only a backstop.
def _run(address: str, scenario: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        # -W ignore silences the DeprecationWarning that os.fork() gives
        # in a process that runs threads, as every process with a client does.
        [sys.executable, "-W", "ignore", "-c", SCRIPT, address, scenario],
        capture_output=True,
        text=True,
        env={"PYTHONPATH": str(PYTHON_DIR)},
        timeout=60,
    )


def test_child_forked_after_a_client_refuses_calls(server):
    result = _run(server, "client-before-fork")
    assert result.returncode == 0, result.stderr


def test_child_forked_before_any_client_works(server):
    result = _run(server, "no-client-before-fork")
    assert result.returncode == 0, result.stderr
