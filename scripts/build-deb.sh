#!/bin/sh
# Build the ds-service Debian package.
#
# Usage: scripts/build-deb.sh [-v VERSION] [-o OUTPUT_DIR]
#
#   -v VERSION     package version, overriding the one from debian/changelog
#   -o OUTPUT_DIR  where the .deb and friends land (default: build/deb)

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$HERE/.." && pwd)

VERSION=""
OUTPUT_DIR="$REPO_ROOT/build/deb"

while getopts "v:o:h" opt; do
    case "$opt" in
        v) VERSION=$OPTARG ;;
        o) OUTPUT_DIR=$OPTARG ;;
        h) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "usage: $0 [-v VERSION] [-o OUTPUT_DIR]" >&2; exit 2 ;;
    esac
done

for tool in conan dpkg-buildpackage cmake; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$0: $tool not found on PATH" >&2
        exit 1
    }
done

STAGE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ds-service-deb.XXXXXX")
trap 'rm -rf "$STAGE_DIR"' EXIT

SRC_DIR="$STAGE_DIR/ds-service"
mkdir -p "$SRC_DIR"

# Ship the committed tree only
git -C "$REPO_ROOT" archive --format=tar HEAD | tar -x -C "$SRC_DIR"

# Fall back to the working copy of debian/ while the packaging is uncommitted.
if [ ! -d "$SRC_DIR/debian" ]; then
    cp -r "$REPO_ROOT/debian" "$SRC_DIR/debian"
fi

# The changelog of the staged tree is what dpkg-buildpackage reads,
# so an explicit -v has to be written into it;
# otherwise it is simply the version being built.
# scripts/update-version.sh is what sets it.
if [ -n "$VERSION" ] ; then
    # sed reports success even when it matches nothing,
    # so check the entry is there before rewriting its version.
    grep -q '^ds-service (.*)' "$SRC_DIR/debian/changelog"
    sed -i "1s/^ds-service (.*)/ds-service ($VERSION)/" "$SRC_DIR/debian/changelog"
else
    VERSION=$(dpkg-parsechangelog -l "$SRC_DIR/debian/changelog" -S Version)
fi

echo "$0: building ds-service $VERSION"

(cd "$SRC_DIR" && dpkg-buildpackage -us -uc -b)

mkdir -p "$OUTPUT_DIR"
find "$STAGE_DIR" -maxdepth 1 -type f -exec cp {} "$OUTPUT_DIR/" \;

echo
echo "Artifacts written to $OUTPUT_DIR:"
ls -1 "$OUTPUT_DIR"
