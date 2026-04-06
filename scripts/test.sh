#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build-test"

cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR/.." -DBUILD_TESTS=ON > /dev/null

TARGETS=(test_inline_hook test_toyhook test_plt_hook test_trace)
cmake --build "$BUILD_DIR" ${TARGETS/#/--target } > /dev/null

FAIL=0
for t in "${TARGETS[@]}"; do
    echo "[$t]"
    "$BUILD_DIR/$t" "$@" || FAIL=1
    echo ""
done

if [ "$FAIL" -eq 0 ]; then
    echo "all tests passed."
else
    echo "some tests failed."
    exit 1
fi
