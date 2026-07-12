#!/usr/bin/env bash
#
# One-shot build for the OceanLabz ESP32-S3-WROOM CAM board (Mode B).
#
# Does EVERYTHING in a clean environment: drops any active Python venv / stale
# ESP-IDF env, installs ESP-IDF v5.5.4 if missing, activates it, installs the
# board-manager helper, selects the board, and builds.
#
# Usage (from anywhere):
#     bash scripts/build_oceanlabz.sh [OPENAI_API_KEY]
#
# The OpenAI key is optional — pass it to bake it into the build, or set it
# later with `idf.py menuconfig`. It can also be provided via the
# CAP_IM_VOICE_OPENAI_API_KEY environment variable.
#
# After a successful build, flash with:
#     . ~/esp/esp-idf-v5.5.4/export.sh
#     cd application/edge_agent && idf.py flash monitor

set -eo pipefail

IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf-v5.5.4}"
IDF_TAG="v5.5.4"
OPENAI_KEY="${1:-${CAP_IM_VOICE_OPENAI_API_KEY:-}}"

# Resolve the project dir relative to this script (repo/scripts -> repo/application/edge_agent).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/../application/edge_agent"

echo "==> Step 1/7: leaving any active Python venv / stale ESP-IDF env"
deactivate 2>/dev/null || true
unset VIRTUAL_ENV IDF_PYTHON_ENV_PATH 2>/dev/null || true

echo "==> Step 2/7: ensuring ESP-IDF $IDF_TAG at $IDF_DIR"
if [ ! -f "$IDF_DIR/export.sh" ]; then
    mkdir -p "$(dirname "$IDF_DIR")"
    echo "    cloning (this downloads a few hundred MB, one time)..."
    git clone -b "$IDF_TAG" --recursive https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

echo "==> Step 3/7: installing the ESP32-S3 toolchain (skips what's present)"
"$IDF_DIR/install.sh" esp32s3

echo "==> Step 4/7: activating ESP-IDF"
# shellcheck disable=SC1091
. "$IDF_DIR/export.sh"
idf.py --version

echo "==> Step 5/7: installing the board-manager helper"
pip install -q esp-bmgr-assist

echo "==> Step 6/7: configuring board 'oceanlabz_s3cam' (clean)"
cd "$PROJECT_DIR"
rm -rf build sdkconfig
idf.py set-target esp32s3
idf.py bmgr -c ./boards -b oceanlabz_s3cam

if [ -n "$OPENAI_KEY" ]; then
    echo "    baking in the OpenAI API key"
    # Remove any prior line, then set it in the generated sdkconfig.
    sed -i.bak '/^CONFIG_CAP_IM_VOICE_OPENAI_API_KEY=/d' sdkconfig 2>/dev/null || true
    echo "CONFIG_CAP_IM_VOICE_OPENAI_API_KEY=\"$OPENAI_KEY\"" >> sdkconfig
    rm -f sdkconfig.bak
fi

echo "==> Step 7/7: building"
idf.py build

echo ""
echo "============================================================"
echo " BUILD OK."
if [ -z "$OPENAI_KEY" ]; then
    echo " NOTE: no OpenAI key was set. Voice STT/TTS will not work"
    echo "       until you set it: idf.py menuconfig -> Voice IM Channel."
fi
echo " To flash the board (in this same terminal):"
echo "     cd \"$PROJECT_DIR\""
echo "     idf.py flash monitor"
echo "============================================================"
