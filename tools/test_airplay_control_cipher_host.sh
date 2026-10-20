#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -z "${IDF_PATH:-}" ] || [ ! -f "$IDF_PATH/components/mbedtls/mbedtls/CMakeLists.txt" ]; then
    echo "Set IDF_PATH to an ESP-IDF checkout before running this host test." >&2
    exit 2
fi

tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/airplay-control-cipher.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

cmake -S "$IDF_PATH/components/mbedtls/mbedtls" -B "$tmp_build/mbedtls" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$tmp_build/mbedtls" --target mbedcrypto --parallel 4

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/tools/test_support/airplay_control_host_stubs" \
    -I"$repo_root/build/config" \
    -I"$repo_root/components/airplay_control/include" \
    -I"$repo_root/components/airplay_control" \
    -I"$repo_root/components/airplay_rtsp/include" \
    -I"$IDF_PATH/components/esp_common/include" \
    -I"$IDF_PATH/components/mbedtls/mbedtls/include" \
    -I"$IDF_PATH/components/mbedtls/mbedtls/library" \
    "$repo_root/components/airplay_control/airplay_control_cipher.c" \
    "$repo_root/components/airplay_control/airplay_control_port.c" \
    "$repo_root/components/airplay_control/airplay_control_stream.c" \
    "$repo_root/components/airplay_control/airplay_command_plist.c" \
    "$repo_root/components/airplay_control/airplay_hid_plist.c" \
    "$repo_root/components/airplay_rtsp/airplay_rtsp_codec.c" \
    "$repo_root/components/airplay_rtsp/airplay_bplist.c" \
    "$repo_root/tools/test_airplay_control_cipher.c" \
    "$tmp_build/mbedtls/library/libmbedcrypto.a" \
    -o "$tmp_build/test_airplay_control_cipher"
"$tmp_build/test_airplay_control_cipher"
