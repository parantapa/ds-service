#!/bin/sh
# Build the ds-service Debian package.
#
# The version comes from the git tags via setuptools_scm,
# and is written into the changelog of the tree being built.
#
# Usage: scripts/build-deb.sh [-v VERSION] [-o OUTPUT_DIR]
#
#   -v VERSION     package version, overriding the one from setuptools_scm
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

if [ -z "$VERSION" ] ; then
    python -c "import setuptools_scm" 2>/dev/null || {
        echo "$0: setuptools_scm is not installed (pip install setuptools_scm)" >&2
        exit 1
    }
    VERSION=$(cd "$REPO_ROOT" && python -m setuptools_scm)
fi

# dpkg sorts 1.0.3.dev1 above 1.0.3,
# so an off-tag build would look newer than the release it precedes.
# '~' sorts below everything, which is what a pre-release wants.
DEB_VERSION=$(printf '%s' "$VERSION" | sed 's/\.dev/~dev/')

echo "$0: building ds-service $DEB_VERSION"

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

# sed reports success even when it matches nothing,
# so check the entry is there before rewriting its version.
grep -q '^ds-service (.*)' "$SRC_DIR/debian/changelog"
sed -i "1s/^ds-service (.*)/ds-service ($DEB_VERSION)/" "$SRC_DIR/debian/changelog"

(cd "$SRC_DIR" && dpkg-buildpackage -us -uc -b)

mkdir -p "$OUTPUT_DIR"
find "$STAGE_DIR" -maxdepth 1 -type f -exec cp {} "$OUTPUT_DIR/" \;

echo
echo "Artifacts written to $OUTPUT_DIR:"
ls -1 "$OUTPUT_DIR"
