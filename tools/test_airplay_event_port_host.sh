#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stub_root="$repo_root/tools/test_support/airplay_event_port_host_stubs"
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/airplay-event-port-host.XXXXXX")
trap 'rm -rf "$temp_dir"' EXIT

cc -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$stub_root" \
    -I"$repo_root/components/airplay_control/include" \
    -I"$repo_root/components/airplay_pairing/include" \
    -I"$repo_root/components/airplay_rtsp/include" \
    "$repo_root/components/airplay_control/airplay_event_port.c" \
    "$repo_root/tools/test_support/airplay_event_port_host_stubs/airplay_control_port_stub.c" \
    "$repo_root/tools/test_airplay_event_port.c" \
    -o "$temp_dir/test_airplay_event_port"
"$temp_dir/test_airplay_event_port"
