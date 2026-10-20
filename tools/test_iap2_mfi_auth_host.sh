#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/iap2-mfi-auth.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/iap2_mfi_auth/include" \
    "$repo_root/components/iap2_mfi_auth/iap2_mfi_auth.c" \
    "$repo_root/tools/test_iap2_mfi_auth.c" \
    -o "$tmp_build/test_iap2_mfi_auth"
"$tmp_build/test_iap2_mfi_auth"
