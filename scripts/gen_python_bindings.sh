#!/bin/bash
# Regenerate the committed Python stubs from cpp/grpc/ds-service.proto.
#
# Takes no arguments, and runs from the repository root.
# Copies the proto into python/ds_service_client,
# then writes ds_service_pb2.py, ds_service_pb2.pyi
# and ds_service_pb2_grpc.py beside the copy.
# Needs grpcio-tools installed.
# Exits non-zero if the copy or protoc fails.
# For when to run it, see "Generated code" in docs/developer-notes.md.
#
# Usage: scripts/gen_python_bindings.sh

set -Eeuo pipefail
set -x

cp -f cpp/grpc/ds-service.proto python/ds_service_client

cd python

python -m grpc_tools.protoc \
    --proto_path=. \
    --python_out=. \
    --pyi_out=. \
    --grpc_python_out=. \
    ds_service_client/ds-service.proto


