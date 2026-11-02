#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="$root/tests/test_runner_keywords"
trap 'rm -f "$test_binary"' EXIT

cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
   -D_DEFAULT_SOURCE "$root/tests/test_runner_keywords.c" \
   -o "$test_binary"
"$test_binary"
