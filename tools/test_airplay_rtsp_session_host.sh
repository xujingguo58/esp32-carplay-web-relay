#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -z "${IDF_PATH:-}" ] || [ ! -f "$IDF_PATH/components/mbedtls/mbedtls/CMakeLists.txt" ]; then
    echo "Set IDF_PATH to an ESP-IDF checkout before running this host test." >&2
    exit 2
fi

tmp_build=$(mktemp -d "${TMPDIR:-/tmp}/airplay-rtsp-session.XXXXXX")
trap 'rm -rf "$tmp_build"' EXIT HUP INT TERM

cmake -S "$IDF_PATH/components/mbedtls/mbedtls" -B "$tmp_build/mbedtls" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$tmp_build/mbedtls" --target mbedcrypto --parallel 4

clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_root/components/airplay_pairing/include" \
    -I"$repo_root/components/airplay_rtsp/include" \
    -I"$repo_root/components/airplay_session/include" \
    -I"$IDF_PATH/components/mbedtls/mbedtls/include" \
    -I"$IDF_PATH/components/mbedtls/mbedtls/library" \
    "$repo_root/components/airplay_pairing/airplay_tlv8.c" \
    "$repo_root/components/airplay_pairing/airplay_srp6a.c" \
    "$repo_root/components/airplay_pairing/airplay_pair_setup.c" \
    "$repo_root/components/airplay_pairing/airplay_pair_verify.c" \
    "$repo_root/components/airplay_rtsp/airplay_rtsp_codec.c" \
    "$repo_root/components/airplay_rtsp/airplay_rtsp_pairing.c" \
    "$repo_root/components/airplay_rtsp/airplay_bplist.c" \
    "$repo_root/components/airplay_session/airplay_rtsp_session.c" \
    "$repo_root/tools/test_airplay_rtsp_session.c" \
    "$tmp_build/mbedtls/library/libmbedcrypto.a" \
    -o "$tmp_build/test_airplay_rtsp_session"
"$tmp_build/test_airplay_rtsp_session"
