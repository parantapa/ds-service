#!/bin/bash

set -Eeuo pipefail

PROJECT="ds-service"
BUILD_ROOT="$HOME/scratch/$PROJECT/build"
BUILD_DIR="$BUILD_ROOT/build/Release"

cmake_configure() {
    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanbuild.sh"
    set -Eeuo pipefail

    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_CXX_FLAGS="-g3 -Wall -Wextra" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_TOOLCHAIN_FILE="generators/conan_toolchain.cmake"
}

cmake_build() {
    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanbuild.sh"
    set -Eeuo pipefail

    cmake --build "$BUILD_DIR" --parallel
}

cmake_install() {
    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanbuild.sh"
    set -Eeuo pipefail

    cmake --install "$BUILD_DIR" --prefix "$1"
}

run_setup() {
    rm -rf "$BUILD_ROOT"
    rm -f compile_commands.json

    conan install . --build=missing --output-folder="$BUILD_ROOT"

    ln -s "$BUILD_DIR/compile_commands.json"

    cmake_configure
    cmake_build
}

run_test() {
    cmake_build

    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanrun.sh"
    set -Eeuo pipefail

    PATH="$BUILD_DIR:$PATH"

    which ds-service
    python -m pytest
}

run_build-static-binary() {
    set -x
    docker build -f scripts/Dockerfile --output type=local,dest=./dist .
}

run_build-python-package() {
    set -x
    python -m build
    python -m twine check dist/*.tar.gz dist/*.whl
}

run_upload-python-package() {
    set -x
    python -m twine upload dist/*.tar.gz dist/*.whl
}

run_release-ds-service() {
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
    # but loses it in the PEP 440 name of a built package,
    # so 2.3.0-rc1 has to match ds_service_client-2.3.0rc1 as well.
    local pyversion="${version//-/}"

    local versions=("$version")
    if [[ "$pyversion" != "$version" ]]; then
        versions+=("$pyversion")
    fi

    # The sdist name is fixed, so it is tested rather than globbed;
    # only the wheel has a trailing build tag to match.
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

show_help() {
    echo "Usage: $0 (help | command)"
}

if [[ "$1" == "help" || "$1" == "-h" || "$1" == "--help" ]]; then
    show_help
elif [[ $(type -t "run_${1}") == function ]]; then
    fn="run_${1}"
    shift
    $fn "$@"
else
    echo "Unknown command: $1"
fi
