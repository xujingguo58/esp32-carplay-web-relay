#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${CC:-cc}
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT INT TERM

"$compiler" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_dir/components/media_bridge/include" \
    "$repo_dir/components/media_bridge/media_timestamp.c" \
    "$repo_dir/tools/test_media_timestamp.c" \
    -o "$temp_dir/test_media_timestamp"
"$temp_dir/test_media_timestamp"
