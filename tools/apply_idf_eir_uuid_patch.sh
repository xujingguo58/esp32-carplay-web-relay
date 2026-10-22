#!/bin/sh
set -eu

if [ -z "${IDF_PATH:-}" ]; then
    echo "IDF_PATH is unset; source ESP-IDF export.sh first." >&2
    exit 2
fi

expected_commit="a9de5ec5b9"
actual_commit=$(git -C "$IDF_PATH" rev-parse --short=10 HEAD)
if [ "$actual_commit" != "$expected_commit" ]; then
    echo "Expected ESP-IDF commit $expected_commit, found $actual_commit; refusing to patch." >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
patch_file="$script_dir/patches/esp-idf-5.5.3-eir-uuid128-little-endian.patch"
if git -C "$IDF_PATH" diff --quiet -- components/bt/host/bluedroid/bta/dm/bta_dm_act.c; then
    git -C "$IDF_PATH" apply "$patch_file"
    echo "Applied the ESP-IDF 5.5.3 EIR UUID128 byte-order patch."
else
    if git -C "$IDF_PATH" diff -- components/bt/host/bluedroid/bta/dm/bta_dm_act.c | grep -q 'ARRAY_TO_BE_STREAM_REVERSE(p, bta_dm_cb.custom_uuid'; then
        echo "The ESP-IDF EIR UUID128 patch is already applied."
    else
        echo "The target ESP-IDF source has other local changes; refusing to patch." >&2
        exit 2
    fi
fi
