#!/bin/sh
set -eu

build_dir="$PWD/build"
mkdir -p "$build_dir"
sdkconfig_path="$build_dir/sdkconfig.ci"
: > "$sdkconfig_path"

IDF_TARGET=esp32 \
SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults.8mb" \
idf.py -B "$build_dir" -D SDKCONFIG="$sdkconfig_path" build

for script in tools/*_host.sh; do
    echo "==> $script"
    bash "$script"
done

node tools/test_web_gateway_page.mjs
python3 tools/validate_test_h264.py
