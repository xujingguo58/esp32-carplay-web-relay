#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/airplay-screen-lifecycle.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/airplay_screen/include" \
    "$repo_root/tools/test_airplay_screen_lifecycle.c" \
    -o "$tmp_build/test_airplay_screen_lifecycle"
"$tmp_build/test_airplay_screen_lifecycle"
echo "airplay screen lifecycle host test passed"
