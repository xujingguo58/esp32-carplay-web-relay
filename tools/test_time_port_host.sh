#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/time-port.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

${CC:-clang} -std=c11 -Wall -Wextra -Werror -g \
    -I"$repo_root/tools/test_support/time_port_host_stubs" \
    -I"$repo_root/components/time_port/include" \
    "$repo_root/components/time_port/time_port.c" \
    "$repo_root/tools/test_time_port.c" \
    -o "$tmp_build/test_time_port"
"$tmp_build/test_time_port"
