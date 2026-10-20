#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

cc -std=c11 -Wall -Wextra -Werror -DED25519_NO_SEED \
    -I"$repo_root/components/ed25519/upstream" \
    "$repo_root/components/ed25519/upstream/fe.c" \
    "$repo_root/components/ed25519/upstream/ge.c" \
    "$repo_root/components/ed25519/upstream/keypair.c" \
    "$repo_root/components/ed25519/upstream/sc.c" \
    "$repo_root/components/ed25519/upstream/sha512.c" \
    "$repo_root/components/ed25519/upstream/sign.c" \
    "$repo_root/components/ed25519/upstream/verify.c" \
    "$repo_root/tools/test_ed25519_host.c" \
    -o "$tmp_dir/test_ed25519"
"$tmp_dir/test_ed25519"
