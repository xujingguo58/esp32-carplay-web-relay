#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/airplay-pair-store.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

${CC:-clang} -std=c11 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -pthread \
    -I"$repo_root/tools/test_support/pairing_store_host_stubs" \
    -I"$repo_root/components/airplay_pairing_store/include" \
    -I"$repo_root/components/airplay_pairing/include" \
    "$repo_root/components/airplay_pairing_store/airplay_pairing_store.c" \
    "$repo_root/tools/test_airplay_pairing_store.c" \
    -o "$tmp_build/test_airplay_pairing_store"
"$tmp_build/test_airplay_pairing_store"
