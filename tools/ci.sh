#!/bin/sh
set -eu

sdkconfig_path=$(mktemp /tmp/esp32-carplay-sdkconfig.XXXXXX)
trap 'rm -f "$sdkconfig_path"' EXIT HUP INT TERM

IDF_TARGET=esp32 \
SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults.8mb" \
idf.py -B build -D SDKCONFIG="$sdkconfig_path" build

for script in tools/*_host.sh; do
    echo "==> $script"
    bash "$script"
done

node tools/test_web_gateway_page.mjs
python3 tools/validate_test_h264.py
