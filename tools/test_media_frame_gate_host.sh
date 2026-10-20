#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/media-frame-gate.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$repo_root/components/media_bridge/include" \
    "$repo_root/tools/test_media_frame_gate.c" \
    -o "$tmp_build/test_media_frame_gate"
"$tmp_build/test_media_frame_gate"
echo "media frame gate host test passed"
