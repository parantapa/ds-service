"""Tests that importing the Python package imports neither grpc nor protobuf.

See "The transport boundary" in docs/developer-notes.md.
"""

import subprocess
import sys
from pathlib import Path

PYTHON_DIR = Path(__file__).resolve().parents[1] / "python"

# A fresh interpreter,
# because another test in this process can import grpc on its own account.
CHECK = """
import sys
import ds_service_client
leaked = sorted(
    name for name in sys.modules
    if name == "grpc" or name.startswith(("grpc.", "google.protobuf"))
)
print(" ".join(leaked))
"""


def test_importing_the_package_imports_no_grpc_or_protobuf():
    result = subprocess.run(
        [sys.executable, "-c", CHECK],
        capture_output=True,
        text=True,
        check=True,
        env={"PYTHONPATH": str(PYTHON_DIR)},
        timeout=30,
    )
    assert result.stdout.strip() == ""
