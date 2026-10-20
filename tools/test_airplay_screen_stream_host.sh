#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -z "${IDF_PATH:-}" ] || [ ! -f "$IDF_PATH/components/mbedtls/mbedtls/CMakeLists.txt" ]; then
    echo "Set IDF_PATH to an ESP-IDF checkout before running this host test." >&2
    exit 2
fi

tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/airplay-screen-stream.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

cmake -S "$IDF_PATH/components/mbedtls/mbedtls" -B "$tmp_build/mbedtls" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$tmp_build/mbedtls" --target mbedcrypto --parallel 4

compiler=${CC:-clang}
"$compiler" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,-dead_strip \
    -DCONFIG_MBEDTLS_CHACHAPOLY_C=1 -DCONFIG_MBEDTLS_HKDF_C=1 \
    -I"$repo_root/tools/test_support/airplay_screen_host_stubs" \
    -I"$repo_root/build/config" \
    -I"$repo_root/components/airplay_screen/include" \
    -I"$repo_root/components/airplay_media_bridge/include" \
    -I"$repo_root/components/airplay_session/include" \
    -I"$repo_root/components/airplay_rtsp/include" \
    -I"$repo_root/components/airplay_pairing/include" \
    -I"$repo_root/components/media_bridge/include" \
    -I"$IDF_PATH/components/esp_common/include" \
    -I"$IDF_PATH/components/mbedtls/mbedtls/include" \
    -I"$IDF_PATH/components/mbedtls/mbedtls/library" \
    "$repo_root/components/airplay_screen/airplay_screen_codec.c" \
    "$repo_root/components/airplay_screen/airplay_screen_stream.c" \
    "$repo_root/components/airplay_media_bridge/airplay_media_bridge.c" \
    "$repo_root/tools/test_airplay_screen_stream.c" \
    "$tmp_build/mbedtls/library/libmbedcrypto.a" \
    -o "$tmp_build/test_airplay_screen_stream"
"$tmp_build/test_airplay_screen_stream"
