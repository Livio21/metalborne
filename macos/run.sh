#!/usr/bin/env bash
# Defaults for this workspace's extracted game and downloaded macOS driver.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
if [[ $(uname -s) != Darwin ]]; then
    echo 'This launcher requires macOS.' >&2
    exit 1
fi
export BB_GAME_DIR=${BB_GAME_DIR:-$PWD/out/game/CUSA03173}
export BB_DATA_DIR=${BB_DATA_DIR:-$PWD/out/macos-run}
export BB_PROBE=${BB_PROBE:-$PWD/out/bb-probe}
export BB_PREBUILT=1
export BB_MODS_ENABLED=${BB_MODS_ENABLED:-0}
export BB_FPS=${BB_FPS:-30}
export BB_PATCHES="${BB_PATCHES-Skip Intro + warning message}"
# Vsync queue for the 30 FPS target. An explicit mode still overrides it.
export BB_PRESENT_MODE=${BB_PRESENT_MODE:-Fifo}
export BB_UPSCALER=${BB_UPSCALER:-off}
export BB_INPUT_MODE=${BB_INPUT_MODE:-auto}
# Temporary workaround for PM4 stream assertions observed during map loading.
# Keep the optimized paths available for profiling as the port matures.
if [[ ${BB_MACOS_CONSERVATIVE_GPU:-1} == 1 ]]; then
    export BB_PREP_WORKERS=${BB_PREP_WORKERS:-0}
    if [[ -z ${BB_TOGGLE_FILE:-} ]]; then
        mkdir -p -- "$BB_DATA_DIR"
        export BB_TOGGLE_FILE=$BB_DATA_DIR/macos-gpu-toggles.txt
        # BbToggle::DrawPipeline (1 << 37).
        printf '137438953472\n' > "$BB_TOGGLE_FILE"
    fi
fi
if [[ -z ${VK_DRIVER_FILES:-} ]]; then
    driver=$PWD/out/vendor/kosmickrisp-0.19.0/kosmickrisp_mesa_icd.json
    if [[ ! -f $driver ]]; then
        echo 'Set VK_DRIVER_FILES to an x86-64 macOS Vulkan driver ICD.' >&2
        exit 1
    fi
    export VK_DRIVER_FILES=$driver
fi
if [[ ! -x $BB_PROBE ]]; then
    echo 'Build the macOS runtime first with bash build.sh.' >&2
    exit 1
fi
exec bash run.sh "$@"
