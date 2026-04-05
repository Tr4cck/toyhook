#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build-test"

cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR/.." -DBUILD_TESTS=ON > /dev/null
cmake --build "$BUILD_DIR" --target test_inline_hook --target test_toyhook --target test_plt_hook > /dev/null

echo "inline_hook tests:"
"$BUILD_DIR/test_inline_hook" "$@"

echo ""
echo "toyhook framework tests:"
"$BUILD_DIR/test_toyhook" "$@"

echo ""
echo "PLT hook tests:"
"$BUILD_DIR/test_plt_hook" "$@"
