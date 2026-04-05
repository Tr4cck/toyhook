#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$SCRIPT_DIR/../toyhook.conf"

[[ -f "$CONF" ]] && source <(grep -v '^\s*#' "$CONF" | grep '=')

NDK="${ANDROID_NDK_ROOT:-${NDK:-}}"
REMOTE_DIR="/data/local/tmp"
PORT=1234

usage() {
    echo "usage: $0 <target_pid> [--ndk <path>]"
    echo ""
    echo "  target_pid    process to inject into"
    echo "  --ndk         path to Android NDK (default: \$ANDROID_NDK_ROOT or \$NDK)"
    exit 1
}

TARGET_PID=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --ndk)  NDK="$2"; shift 2 ;;
        -h|--help) usage ;;
        *)      TARGET_PID="$1"; shift ;;
    esac
done

[[ -z "$TARGET_PID" ]] && usage
[[ -z "$NDK" ]] && { echo "[-] set \$ANDROID_NDK_ROOT or pass --ndk <path>"; exit 1; }

# --- build + push ---
"$SCRIPT_DIR/build.sh" --ndk "$NDK" --push

# --- lldb-server ---
LLDB_SERVER="$NDK/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/21/lib/linux/aarch64/lldb-server"
if ! adb shell ls "$REMOTE_DIR/lldb-server" &> /dev/null; then
    echo "[*] pushing lldb-server..."
    adb push "$LLDB_SERVER" "$REMOTE_DIR/"
else
    echo "[*] lldb-server already exists on device, skipping push"
fi

# --- start toyhook (detached) ---
echo "[*] starting toyhook on device..."
adb shell su -c "nohup env TOYHOOK_WAIT_DEBUGGER=1 \
    $REMOTE_DIR/toyhook inject $TARGET_PID $REMOTE_DIR/libtoyhook_payload.so \
    >/dev/null 2>&1 &"

sleep 1
TOYHOOK_PID=$(adb shell pidof toyhook | tr -d '\r')
if [[ -z "$TOYHOOK_PID" ]]; then
    echo "[-] toyhook not started, check logcat"
    exit 1
fi
echo "[+] toyhook pid: $TOYHOOK_PID"

# --- port forward ---
echo "[*] setting up port forward :$PORT"
adb forward tcp:$PORT tcp:$PORT

# --- attach lldb-server ---
echo "[*] attaching lldb-server to toyhook..."
echo "[*] now press F5 in VSCode to connect"
echo "[*] log: adb logcat -s toyhook"
echo "---"
adb shell su -c "$REMOTE_DIR/lldb-server g :$PORT --attach $TOYHOOK_PID"
