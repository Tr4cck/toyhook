#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

NDK="${ANDROID_NDK_ROOT:-${NDK:-}}"
REMOTE_DIR="/data/local/tmp"

usage() {
    echo "usage: $0 [--ndk <path>] [--push]"
    echo ""
    echo "  --ndk   path to Android NDK (default: \$ANDROID_NDK_ROOT)"
    echo "  --push  push binaries to device after build"
    exit 1
}

DO_PUSH=0
while [[ $# -gt 0 ]]; do
    case $1 in
        --ndk)   NDK="$2"; shift 2 ;;
        --push)  DO_PUSH=1; shift ;;
        -h|--help) usage ;;
        *)       usage ;;
    esac
done

[[ -z "$NDK" ]] && { echo "[-] set \$ANDROID_NDK_ROOT or pass --ndk <path>"; exit 1; }

TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[[ ! -f "$TOOLCHAIN" ]] && { echo "[-] toolchain not found: $TOOLCHAIN"; exit 1; }

echo "[*] building..."
BUILD_DIR="$SCRIPT_DIR/../build"
cmake -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-28 \
    -DCMAKE_BUILD_TYPE=Debug \
    -S "$SCRIPT_DIR/.."
cmake --build "$BUILD_DIR"

echo "[+] built: toyhook libtoyhook_payload.so toyhookctl"

if [[ "$DO_PUSH" -eq 1 ]]; then
    echo "[*] pushing to device..."
    adb push "$BUILD_DIR/toyhook" "$REMOTE_DIR/"
    adb push "$BUILD_DIR/libtoyhook_payload.so" "$REMOTE_DIR/"
    adb push "$BUILD_DIR/toyhookctl" "$REMOTE_DIR/"
    echo "[+] pushed to $REMOTE_DIR/"
fi
