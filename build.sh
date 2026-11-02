#!/bin/sh
# Build this firmware with fm1-base (base/, a git submodule).
#   ./build.sh [--release X.Y] [--no-ble] [--host | --web]
#   JIELI_TOOLCHAIN  JieLi Linux toolchain (default: ~/.jieli/toolchain)
#   AC79_SDK         JieLi AC79 SDK checkout (default: ~/fw-AC79_AIoT_SDK)
#   PYTHON           a Python with Pillow (and libraqm, for the fonts)
#   FRIBIDI_LIB      macOS: a folder with libfribidi.0.dylib for Pillow's raqm, when the system's is the wrong
#                    architecture (default: ~/.local/fribidi-arm64/lib, if it is there)
#   EMSDK            Emscripten for --web, when emcc is not on the PATH (default: ~/emsdk, if it is there)
set -e
cd "$(dirname "$0")"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
PY="${PYTHON:-python3}"
[ -f base/tools/build.py ] || { echo "build.sh: no fm1-base in base/ (git submodule update --init --recursive)"; exit 1; }
"$PY" -c 'import PIL' 2>/dev/null || { echo "build.sh: $PY has no Pillow (pip3 install Pillow)"; exit 1; }
FRIBIDI_LIB="${FRIBIDI_LIB:-$HOME/.local/fribidi-arm64/lib}"
if [ "$(uname -s)" = Darwin ] && [ -f "$FRIBIDI_LIB/libfribidi.0.dylib" ]; then
    # /usr/bin/python3 is a system shim that drops DYLD_* variables: run the Python behind it
    PY="$("$PY" -c 'import sys; print(sys.executable)')"
    export DYLD_FALLBACK_LIBRARY_PATH="$FRIBIDI_LIB${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
fi
"$PY" -c 'from PIL import features; assert features.check("raqm")' 2>/dev/null ||
    { echo "build.sh: $PY's Pillow has no raqm (its fonts need FriBiDi: see FRIBIDI_LIB)"; exit 1; }
if ! command -v emcc >/dev/null 2>&1 && [ -f "${EMSDK:-$HOME/emsdk}/emsdk_env.sh" ]; then
    . "${EMSDK:-$HOME/emsdk}/emsdk_env.sh" >/dev/null 2>&1
fi
exec "$PY" base/tools/build.py app "$@"
