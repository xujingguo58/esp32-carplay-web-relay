#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/test-touch-hid-bridge.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/input_bridge/include" \
    -I"$repo_root/components/airplay_control/include" \
    -I"$repo_root/components/airplay_rtsp/include" \
    "$repo_root/components/input_bridge/input_bridge.c" \
    "$repo_root/components/airplay_control/airplay_hid_plist.c" \
    "$repo_root/components/airplay_control/airplay_command_plist.c" \
    "$repo_root/components/airplay_rtsp/airplay_bplist.c" \
    "$repo_root/tools/test_touch_hid_bridge.c" \
    -lm -o "$test_binary"
"$test_binary"
