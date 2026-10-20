#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/test-input-bridge.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/input_bridge/include" \
    "$repo_root/components/input_bridge/input_bridge.c" \
    "$repo_root/tools/test_input_bridge.c" \
    -o "$test_binary"
"$test_binary"
