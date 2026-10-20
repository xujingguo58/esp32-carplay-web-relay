#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/test-airplay-control-stream.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/airplay_control/include" \
    -I"$repo_root/components/airplay_rtsp/include" \
    "$repo_root/components/airplay_control/airplay_control_stream.c" \
    "$repo_root/components/airplay_rtsp/airplay_rtsp_codec.c" \
    "$repo_root/tools/test_airplay_control_stream.c" \
    -o "$test_binary"
"$test_binary"
