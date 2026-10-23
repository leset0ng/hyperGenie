#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/genie-model-tests.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

if test -n "${CXX:-}"; then
    set -- "$CXX"
elif command -v xcrun >/dev/null 2>&1; then
    # xcrun supplies the SDK as well as selecting the host compiler; PATH may
    # otherwise select the Android NDK compiler, which cannot run host tests.
    set -- xcrun clang++
else
    set -- clang++
fi

"$@" -std=c++17 -Wall -Wextra -Werror \
    -I "$project_dir/app/src/main/cpp" \
    "$project_dir/app/src/main/cpp/genie_model.cpp" \
    "$project_dir/tests/genie_test.cpp" \
    -o "$test_dir/genie_test"
"$test_dir/genie_test"
