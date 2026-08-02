# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Read the docs first

The user-facing documentation is the source of truth for everything it
covers; it is deliberately not repeated here. Read the relevant file
before answering questions or changing code in that area:

- `README.md` — what `ds-service` is, the six data structures it
    provides, and how server / client / proto fit together.
- `docs/data-structure-reference.md` — every RPC, its arguments, its
    error statuses, and the exact semantics of each structure.
- `docs/howto-build-the-server.md` — requirements, the Conan + CMake
    build, installing, running, and the container build.
- `docs/howto-run-the-tests.md` — the pytest integration suite,
    `DS_SERVICE_BIN`, and what the fixtures provide.
- `docs/howto-use-the-python-client.md` — installing, connecting, the
    gRPC-status-to-exception mapping, and usage examples.

When behavior changes, update the doc that covers it rather than
restating it here.

## Generated code

`misc/ds-service.proto` is the source of truth for the generated files.
**Never edit a generated file directly** — the Python stubs are committed
and marked "auto generated, do not edit"; the C++ protobuf/gRPC stubs
only exist in the build tree.

A proto change is a four-step job, and only the first two are automatic:

1. Edit `misc/ds-service.proto`.
2. Rebuild the C++ (regenerates `ds-service.pb.*` / `ds-service.grpc.pb.*`).
3. Run `scripts/gen_python_bindings.sh` — **manual**; skip it and the
    Python client silently goes stale.
4. Hand-update `cpp/ds-service.cpp` and
    `python/ds_service_client/client.py` to implement and expose the change.

## Conventions

- C++ formatting is enforced by `.clang-format` (LLVM base, 4-space
    indent, 120 columns, left pointer alignment, `SortIncludes: false` —
    include order is intentional).
- Every version string in the repo is set by
    `scripts/update-version.sh <version>`: `cpp/ds-service.cpp`,
    `CMakeLists.txt` (numeric part only — `project(VERSION)` rejects a
    pre-release suffix), `pyproject.toml`, and `conanfile.py`. Set them
    through the script rather than by hand. The script greps for each
    line first and edits nothing unless all of them are present.

## Comments

* When generating block comments, use semantic line breaks.
