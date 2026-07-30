# How to run the tests

The test suite in `tests/` is an integration suite driven by
[pytest](https://pytest.org/).
There are no unit tests of the C++ internals: every test starts a real
`ds-service` process and drives it through the Python client over gRPC.

## Prerequisites

1. **Build the server** -- the tests run the compiled binary.
    See [howto-build-the-server.md](howto-build-the-server.md).
2. **Install the test dependencies:**

    ```sh
    pip install -e ".[test]"
    ```

    This pulls in `pytest`; `grpcio` is already a dependency of the
    client package.
    Installing is not strictly required for the client itself --
    `pyproject.toml` sets `pythonpath = ["python"]`, so
    `ds_service_client` imports straight from the source tree.

## Pointing the tests at the binary

The fixture locates the server binary in one of two ways, in order:

1. `DS_SERVICE_BIN`, if set, is used as an explicit path to the binary.
    Pointing it at a missing file is an error, not a fallback.
2. Otherwise, a `ds-service` found on `PATH`.

It does **not** search the build tree, so after an in-tree build either
set the variable or put the binary on `PATH`:

```sh
export DS_SERVICE_BIN=build/Release/ds-service
```

If neither is available, every test fails with a `FileNotFoundError`
naming both options.

## Running

```sh
python -m pytest                 # everything
python -m pytest tests/test_journal.py
python -m pytest tests/test_tasks.py::test_task_requeue_returns_stalled_task
```

`testpaths = ["tests"]` in `pyproject.toml` means a bare
`python -m pytest` picks up the suite from the repository root.

After changing `misc/ds-service.proto` or the C++ server, rebuild the
binary -- and run `scripts/gen_python_bindings.sh` for a proto change --
before running the suite, or it exercises stale code.

## How the harness works

The fixtures live in `tests/conftest.py`:

| Fixture | Yields |
| --- | --- |
| `server_binary` | The path to the binary under test. |
| `server_process` | `(proc, address)` for a running server -- for tests that drive the process itself, such as signalling it. |
| `server` | The address of a running server. |
| `client` | A connected `Client`, closed at the end of the test. |

Each test gets a **fresh server process on its own free port**, so the
server's in-memory state is isolated between tests and the suite can run
without a fixed port.
Startup waits for the port to accept a TCP connection and then makes one
read-only RPC, which confirms the service is registered and answering;
teardown terminates the process, escalating to a kill if it does not
exit within five seconds.
