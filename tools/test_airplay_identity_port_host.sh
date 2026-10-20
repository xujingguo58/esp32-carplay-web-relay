#!/bin/sh
set -eu

: "${IDF_PATH:?Set IDF_PATH to an ESP-IDF checkout before running this host test.}"
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/test-airplay-identity-port.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/build/config" \
    -I"$IDF_PATH/components/esp_common/include" \
    -I"$repo_root/components/airplay_pairing/include" \
    -I"$repo_root/components/airplay_control/include" \
    -I"$repo_root/components/airplay_identity_port/include" \
    -I"$repo_root/components/airplay_session/include" \
    -I"$repo_root/components/airplay_rtsp/include" \
    "$repo_root/components/airplay_identity_port/airplay_identity_port.c" \
    "$repo_root/components/airplay_control/airplay_hid_descriptor.c" \
    "$repo_root/components/airplay_rtsp/airplay_bplist.c" \
    "$repo_root/tools/test_airplay_identity_port.c" \
    -o "$test_binary"
"$test_binary"
