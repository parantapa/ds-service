#!/bin/bash
# The author's own build wrapper. Not required to build the project.
#
# Builds out of tree, under $HOME/scratch/ds-service/build,
# and runs from the repository root.
# Takes one command, and runs the run_<command> function below.
# Exits 1 on an unknown command,
# and otherwise with the status of the command.
#
# Usage: scripts/pb-dev.sh (help | command)

set -Eeuo pipefail

PROJECT="ds-service"
BUILD_ROOT="$HOME/scratch/$PROJECT/build"
BUILD_DIR="$BUILD_ROOT/build/Release"

# Conan's generated environment scripts do not run under strict mode,
# so each function turns it off while it sources one.

# Configure the build tree, with the Python module turned on.
cmake_configure() {
    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanbuild.sh"
    set -Eeuo pipefail

    # DS_SERVICE_BUILD_PYTHON builds ds_service_client._ext,
    # and copies it into python/ds_service_client for the test suite.
    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_CXX_FLAGS="-g3 -Wall -Wextra" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DDS_SERVICE_BUILD_PYTHON=ON \
        -DCMAKE_TOOLCHAIN_FILE="generators/conan_toolchain.cmake"
}

# Build every target in the build tree.
cmake_build() {
    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanbuild.sh"
    set -Eeuo pipefail

    cmake --build "$BUILD_DIR" --parallel
}

# Install the built server, and the Python module with its stub,
# under the prefix given as the first argument.
cmake_install() {
    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanbuild.sh"
    set -Eeuo pipefail

    cmake --install "$BUILD_DIR" --prefix "$1"
}

# Delete the build tree, install the Conan dependencies,
# link compile_commands.json into the repository root, then build.
run_setup() {
    rm -rf "$BUILD_ROOT"
    rm -f compile_commands.json

    conan install . --build=missing --output-folder="$BUILD_ROOT"

    ln -s "$BUILD_DIR/compile_commands.json"

    cmake_configure
    cmake_build
}

# Rebuild, then run the C++ smoke test and the pytest suite against the fresh build.
run_test() {
    cmake_build

    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanrun.sh"
    set -Eeuo pipefail

    # The suite finds the server on PATH when DS_SERVICE_BIN is unset.
    PATH="$BUILD_DIR:$PATH"

    # Logs which server binary the suite starts,
    # and under set -e stops the run if the build produced none.
    which ds-service
    ctest --test-dir "$BUILD_DIR" --output-on-failure
    python -m pytest
}

# Build the static musl binary and write it to dist/.
run_build-static-binary() {
    set -x
    docker build -f scripts/Dockerfile --output type=local,dest=./dist .
}

# Build the client sdist, and the manylinux wheel, into dist/,
# and check them with twine.
# cibuildwheel builds the wheel in a manylinux container,
# so it needs docker or podman.
run_build-python-package() {
    set -x
    python -m build --sdist
    python -m cibuildwheel --platform linux --output-dir dist
    python -m twine check dist/*.tar.gz dist/*.whl
}

# Upload the client sdist and wheel in dist/ with twine.
run_upload-python-package() {
    set -x
    python -m twine upload dist/*.tar.gz dist/*.whl
}

# Create a GitHub release named after the version of dist/ds-service,
# with the binary, the sdist and the wheel of that version attached.
# Exit 1 if any of them is missing, or if gh is missing or not logged in.
run_make-release() {
    local binary="dist/ds-service"
    local repo="https://github.com/parantapa/ds-service"

    if [[ ! -x "$binary" ]]; then
        echo "Error: $binary not found or not executable" >&2
        exit 1
    fi

    if ! command -v gh >/dev/null 2>&1; then
        echo "Error: github cli (gh) not found" >&2
        exit 1
    fi

    if ! gh auth status >/dev/null 2>&1; then
        echo "Error: not authenticated to github; run 'gh auth login'" >&2
        exit 1
    fi

    local version
    version=$("$binary" --version)
    if [[ -z "$version" ]]; then
        echo "Error: unable to determine version from $binary" >&2
        exit 1
    fi

    # A pre-release suffix keeps its separator in the binary's version
    # but loses it in the PEP 440 name of a built package.
    # So 2.3.0-rc1 must match ds_service_client-2.3.0rc1 as well.
    local pyversion="${version//-/}"

    local versions=("$version")
    if [[ "$pyversion" != "$version" ]]; then
        versions+=("$pyversion")
    fi

    # The sdist name is fixed,
    # so the script tests for it rather than globbing.
    # Only the wheel has a trailing build tag to match.
    local sdists=() whls=() v
    shopt -s nullglob
    for v in "${versions[@]}"; do
        if [[ -f "dist/ds_service_client-$v.tar.gz" ]]; then
            sdists+=("dist/ds_service_client-$v.tar.gz")
        fi
        whls+=(dist/ds_service_client-"$v"-*.whl)
    done
    shopt -u nullglob

    if [[ ${#sdists[@]} -eq 0 ]]; then
        echo "Error: no sdist for version $version in dist/" >&2
        exit 1
    fi
    if [[ ${#whls[@]} -eq 0 ]]; then
        echo "Error: no wheel for version $version in dist/" >&2
        exit 1
    fi

    set -x
    gh release create "v$version" \
        --repo "$repo" \
        --title "v$version" \
        --generate-notes \
        "$binary" "${sdists[@]}" "${whls[@]}"
}

# Print the usage and the list of commands.
show_help() {
    echo "Usage: $0 (help | command)"
    echo
    echo "Available commands:"
    echo "    help"

    # The command list is derived from the run_* functions,
    # so adding a command needs no change here.
    local fn
    while read -r fn; do
        echo "    ${fn#run_}"
    done < <(declare -F | awk '{print $3}' | grep '^run_' | sort)
}

if [[ $# -eq 0 || "${1:-}" == "help" || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    show_help
elif [[ $(type -t "run_${1}") == function ]]; then
    fn="run_${1}"
    shift
    $fn "$@"
else
    echo "Unknown command: $1" >&2
    show_help >&2
    exit 1
fi
