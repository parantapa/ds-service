# How to run the tests

The test suite in `tests/` is an integration suite driven by
[pytest](https://pytest.org/).
Every test starts a real `ds-service` process
and drives it through the Python client over gRPC.

## Prerequisites

1. **Build the server** -- the tests run the compiled binary.
    See [how to build the server](howto-build-the-server.md).
2. **Install the test dependencies:**

    ```sh
    pip install -e ".[test]"
    ```

    This pulls in `pytest`.
    Installing is not strictly required for the client itself --
    `pyproject.toml` sets `pythonpath = ["python"]`,
    so `ds_service_client` imports straight from the source tree.

## Pointing the tests at the binary

The fixtures start the server through
`ds_service_client`'s own `DsServiceServer` helper,
which locates it in one of two ways, in order:

1. `DS_SERVICE_BIN`, if set. It may be a whole command
    -- `docker run --rm --network host ds-service` -- not just a path.
2. Otherwise, a `ds-service` found on `PATH`.

After an in-tree build, point the variable at the binary:

```sh
export DS_SERVICE_BIN=build/Release/ds-service
```

If neither is available,
every test fails with a `FileNotFoundError`.

## Running

```sh
python -m pytest                 # everything
python -m pytest tests/test_journal.py
python -m pytest tests/test_tasks.py::test_add_get_done_lifecycle
```

`testpaths = ["tests"]` in `pyproject.toml`
means a bare `python -m pytest` picks up the suite from the repository root.

After changing `misc/ds-service.proto` or the C++ server,
rebuild the binary -- and run `scripts/gen_python_bindings.sh` for a proto change --
before running the suite, or it exercises stale code.

Each test gets a fresh server process on its own free port,
so no test can see another's state and no fixed port is needed.
For the fixtures themselves,
see the [developer notes](developer-notes.md#the-test-harness).
