#!/bin/bash

set -Eeuo pipefail
set -x

cppsoa -i misc/task_table.json5 -o cpp/task_table.hpp
