#!/bin/bash
# Cross-compile from Linux (needs: mingw-w64 cmake)
set -e
S="$(pwd)/cross-shim"
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-I$S" -DCMAKE_CXX_FLAGS="-I$S"
cmake --build build -j
echo "Done: build/ldac-win.exe"
