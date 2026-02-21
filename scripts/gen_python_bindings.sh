#!/bin/bash

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


