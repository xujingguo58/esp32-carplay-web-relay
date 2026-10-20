#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/airplay-bplist.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/airplay_rtsp/include" \
    "$repo_root/components/airplay_rtsp/airplay_bplist.c" \
    "$repo_root/tools/test_airplay_bplist.c" \
    -o "$tmp_build/test_airplay_bplist"
"$tmp_build/test_airplay_bplist"
