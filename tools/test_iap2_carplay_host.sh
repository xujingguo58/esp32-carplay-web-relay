#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/iap2-carplay.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/iap2_carplay/include" \
    "$repo_root/components/iap2_carplay/iap2_carplay.c" \
    "$repo_root/tools/test_iap2_carplay.c" \
    -o "$tmp_build/test_iap2_carplay"
"$tmp_build/test_iap2_carplay"
