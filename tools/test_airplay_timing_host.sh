#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/airplay-timing.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/airplay_timing/include" \
    "$repo_root/components/airplay_timing/airplay_timing.c" \
    "$repo_root/tools/test_airplay_timing.c" \
    -lm -o "$tmp_build/test_airplay_timing"
"$tmp_build/test_airplay_timing"
