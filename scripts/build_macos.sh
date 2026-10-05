#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
cpu=OFF
if [[ ${1:-} == --cpu-only ]]; then cpu=ON; shift; fi
cmake_bin=${CMAKE:-cmake}
if ! command -v "$cmake_bin" >/dev/null; then
    if [[ -x out/macos-tools/bin/cmake ]]; then cmake_bin=$PWD/out/macos-tools/bin/cmake
    else echo 'Install CMake and Ninja, or set CMAKE to the CMake executable.' >&2; exit 1; fi
fi
if [[ -d out/macos-tools/bin ]]; then export PATH="$PWD/out/macos-tools/bin:$PATH"; fi
build=out/macos
if [[ $cpu == ON ]]; then build=out/macos-cpu; fi
if [[ $cpu == OFF ]]; then
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
        fi
    done
fi
"$cmake_bin" -S macos -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBB_CPU_ONLY="$cpu" -DBB_LTO="${BB_LTO:-ON}" "$@"
"$cmake_bin" --build "$build" --parallel "${BB_BUILD_JOBS:-4}"
