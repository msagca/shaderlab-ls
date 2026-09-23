#!/bin/sh
# Configures and builds shaderlab-ls with the CMake presets. Usage: ./build.sh [Release|Debug]
set -e
config=${1:-Release}
# cmake --build --preset reads the presets from the current directory; it has no -S.
cd "$(dirname "$0")"
cmake --preset "$config"
cmake --build --preset "$config"
