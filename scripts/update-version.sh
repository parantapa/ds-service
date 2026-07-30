#!/bin/bash
# Set the version in cpp/ds-service.cpp, CMakeLists.txt, pyproject.toml,
# conanfile.py and debian/changelog.
#
# Usage: scripts/update-version.sh VERSION

set -Eeuo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 VERSION" >&2
    exit 2
fi

VERSION=$1

set -x

# project(VERSION) only accepts dotted numbers,
# so CMake gets the leading numeric part on its own,
# without any pre-release suffix.
CMAKE_VERSION=$(printf '%s' "$VERSION" | grep -oE '^[0-9]+(\.[0-9]+){0,2}') || {
    set +x
    echo "$0: version '$VERSION' does not start with a number" >&2
    exit 1
}

# sed reports success even when it matches nothing,
# so check every line is there before touching any file;
# a failed check then leaves none of them half updated.
# The project() block is read on its own because
# a bare "VERSION" also appears in cmake_minimum_required().
PROJECT_BLOCK=$(sed -n '/^project(/,/)/p' CMakeLists.txt)

grep -q '^const char\* VERSION = ".*";$' cpp/ds-service.cpp
grep -q '^  VERSION ' <<<"$PROJECT_BLOCK"
grep -q '^version = ".*"$' pyproject.toml
grep -q '^    version = ".*"$' conanfile.py
grep -q '^ds-service (.*)' debian/changelog
grep -q '^ -- .*  ' debian/changelog

sed -i -E "s|^const char\* VERSION = \".*\";$|const char* VERSION = \"${VERSION}\";|" cpp/ds-service.cpp
sed -i -E "/^project\(/,/\)/ s|^  VERSION .*$|  VERSION ${CMAKE_VERSION}|" CMakeLists.txt

# The [project] version is the only unindented one in pyproject.toml,
# and the recipe attribute the only indented one in conanfile.py.
sed -i -E "s|^version = \".*\"$|version = \"${VERSION}\"|" pyproject.toml
sed -i -E "s|^    version = \".*\"$|    version = \"${VERSION}\"|" conanfile.py

# Only the topmost changelog entry belongs to the version being set,
# so the trailer date is rewritten in the first entry alone.
sed -i -E "1s|^ds-service \(.*\)|ds-service (${VERSION})|" debian/changelog
sed -i -E "0,/^ -- /s|^( -- .*)  .*$|\1  $(date -R)|" debian/changelog

grep -n '^const char\* VERSION' cpp/ds-service.cpp
sed -n '/^project(/,/)/p' CMakeLists.txt
grep -n '^version = ' pyproject.toml
grep -n '^    version = ' conanfile.py
head -1 debian/changelog
