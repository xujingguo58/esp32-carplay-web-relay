#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/bt-port-host.XXXXXX")
trap 'rm -rf "$temp_dir"' EXIT

stub_source="$repo_root/tools/test_support/bt_port_host_stubs/bt_port_host_api.h"
for header in esp_err.h esp_bt.h esp_bt_device.h esp_bt_main.h esp_gap_bt_api.h esp_log.h \
              esp_sdp_api.h esp_spp_api.h freertos/FreeRTOS.h \
              freertos/queue.h freertos/task.h; do
    mkdir -p "$temp_dir/$(dirname "$header")"
    ln -s "$stub_source" "$temp_dir/$header"
done

cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-but-set-variable \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$temp_dir" -I"$repo_root/components/bt_port/include" \
    "$repo_root/tools/test_bt_port.c" -o "$temp_dir/test_bt_port"
"$temp_dir/test_bt_port"
