#!/bin/bash
# Regenerate the committed Python stubs from misc/ds-service.proto.
#
# Run this after every proto change.
# The C++ build regenerates its own stubs, but these are checked in.
#
# Usage: scripts/gen_python_bindings.sh

set -Eeuo pipefail
set -x

cp -f misc/ds-service.proto python/ds_service_client

cd python

python -m grpc_tools.protoc \
    --proto_path=. \
    --python_out=. \
    --pyi_out=. \
    --grpc_python_out=. \
    ds_service_client/ds-service.proto


