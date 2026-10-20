#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

python3 "$repo_root/tools/generate_airplay_info_plist.py"
cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo_root/components/airplay_rtsp/include" \
    -I"$repo_root/components/airplay_control/include" \
    "$repo_root/components/airplay_rtsp/airplay_bplist.c" \
    "$repo_root/components/airplay_control/airplay_hid_descriptor.c" \
    "$repo_root/tools/test_airplay_info_plist.c" \
    -o "$tmp_dir/test_airplay_info_plist"
"$tmp_dir/test_airplay_info_plist" \
    "$repo_root/components/airplay_software_provider/assets/info.plist"
