#!/bin/sh
# Configures and builds shaderlab-ls with CMake + Ninja. Usage: ./build.sh [Release|Debug]
set -e
config=${1:-Release}
root=$(cd "$(dirname "$0")" && pwd)
cmake -S "$root" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$root/build"
