#!/bin/bash

set -Eeuo pipefail

. "$HOME/default-env.sh"

PROJECT="ds-service"
BUILD_ROOT="$SCRATCH_DIR/$PROJECT/build"
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

run_setup() {
    rm -rf "$BUILD_ROOT"
    rm -f compile_commands.json

    conan install . --build=missing --output-folder="$BUILD_ROOT"

    ln -s "$BUILD_DIR/compile_commands.json"

    cmake_configure
    cmake_build
}

run_server() {
    cmake_build

    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanrun.sh"
    set -Eeuo pipefail

    PATH="$BUILD_DIR:$PATH"

    which ds-server
    ds-server
}

run_install_in_home() {
    set +Eeuo pipefail
    . "$BUILD_DIR/generators/conanrun.sh"
    set -Eeuo pipefail

    set -x

    cp -a "$BUILD_DIR/ds-server" "$HOME/bin"
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
