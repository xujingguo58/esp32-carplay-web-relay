#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/test-web-gateway-pairing.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/web_gateway" \
    "$repo_root/components/web_gateway/web_gateway_pairing.c" \
    "$repo_root/tools/test_web_gateway_pairing.c" \
    -o "$test_binary"
"$test_binary"
