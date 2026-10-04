#!/bin/bash
# Run inside the "MSYS2 MINGW64" shell:
#   pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
set -e
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo "Done: build/ldac-win.exe"
