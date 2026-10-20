#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/web-gateway-rate-limit.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/web_gateway" \
    "$repo_root/components/web_gateway/web_gateway_rate_limit.c" \
    "$repo_root/tools/test_web_gateway_rate_limit.c" \
    -o "$tmp_build/test_web_gateway_rate_limit"
"$tmp_build/test_web_gateway_rate_limit"
