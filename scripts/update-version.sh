#!/bin/bash
# Set the version in cpp/ds-service.cpp and CMakeLists.txt
# to the version setuptools_scm derives from the git tags.

set -Eeuo pipefail

if ! python -c "import setuptools_scm" 2>/dev/null; then
    echo "$0: setuptools_scm is not installed (pip install setuptools_scm)" >&2
    exit 1
fi

set -x

VERSION=$(python -m setuptools_scm)

# project(VERSION) only accepts dotted numbers,
# so CMake gets the release part on its own,
# without the .devN+gHASH suffix setuptools_scm adds off-tag.
CMAKE_VERSION=$(printf '%s' "$VERSION" | grep -oE '^[0-9]+(\.[0-9]+){0,2}')

# sed reports success even when it matches nothing,
# so check both lines are there before touching either file;
# a failed check then leaves neither of them half updated.
# The project() block is read on its own because
# a bare "VERSION" also appears in cmake_minimum_required().
PROJECT_BLOCK=$(sed -n '/^project(/,/)/p' CMakeLists.txt)

grep -q '^const char\* VERSION = ".*";$' cpp/ds-service.cpp
grep -q '^  VERSION ' <<<"$PROJECT_BLOCK"

sed -i -E "s|^const char\* VERSION = \".*\";$|const char* VERSION = \"${VERSION}\";|" cpp/ds-service.cpp
sed -i -E "/^project\(/,/\)/ s|^  VERSION .*$|  VERSION ${CMAKE_VERSION}|" CMakeLists.txt

grep -n '^const char\* VERSION' cpp/ds-service.cpp
sed -n '/^project(/,/)/p' CMakeLists.txt
