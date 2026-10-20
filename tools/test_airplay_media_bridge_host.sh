#!/bin/sh
set -eu

: "${IDF_PATH:?Set IDF_PATH to an ESP-IDF checkout before running this host test.}"
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/test-airplay-media-bridge.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,-dead_strip \
    -I"$repo_root/build/config" \
    -I"$IDF_PATH/components/esp_common/include" \
    -I"$IDF_PATH/components/mbedtls/mbedtls/include" \
    -I"$repo_root/components/airplay_media_bridge/include" \
    -I"$repo_root/components/airplay_screen/include" \
    -I"$repo_root/components/airplay_session/include" \
    -I"$repo_root/components/airplay_rtsp/include" \
    -I"$repo_root/components/airplay_pairing/include" \
    -I"$repo_root/components/media_bridge/include" \
    "$repo_root/components/airplay_media_bridge/airplay_media_bridge.c" \
    "$repo_root/tools/test_airplay_media_bridge.c" \
    -o "$test_binary"
"$test_binary"
