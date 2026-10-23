#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Isolated Launcher-hook executable; never installs a module or touches real layers.
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ndk_dir=${ANDROID_NDK_HOME:-"$HOME/Library/Android/sdk/ndk/30.0.14904198"}
serial=${ADB_SERIAL:-${ANDROID_SERIAL:-emulator-5580}}
adb_bin=${ADB:-adb}
case $(uname -s) in
    Darwin) host_tag=darwin-x86_64 ;;
    Linux) host_tag=linux-x86_64 ;;
    *) echo 'Unsupported NDK host; use macOS or Linux.' >&2; exit 2 ;;
esac
compiler="$ndk_dir/toolchains/llvm/prebuilt/$host_tag/bin/aarch64-linux-android35-clang++"
if ! test -x "$compiler"; then
    echo "Android NDK API 35 compiler not found: $compiler" >&2
    exit 2
fi

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/genie-launcher-device-tests.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
binary="$test_dir/genie-launcher-device-tests"
"$compiler" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti -static-libstdc++ \
    -I "$project_dir/app/src/main/cpp" \
    "$project_dir/tests/launcher_device_test.cpp" \
    "$project_dir/app/src/main/cpp/genie_model.cpp" \
    -landroid -llog -ldl -pthread -o "$binary"
"$adb_bin" -s "$serial" push "$binary" /data/local/tmp/genie-launcher-device-tests
"$adb_bin" -s "$serial" shell 'chmod 755 /data/local/tmp/genie-launcher-device-tests && timeout 15 /data/local/tmp/genie-launcher-device-tests'
