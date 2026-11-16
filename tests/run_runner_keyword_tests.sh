#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="$root/tests/test_runner_keywords"
trap 'rm -f "$test_binary"' EXIT

cd "$root"
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
   -D_DEFAULT_SOURCE "$root/tests/test_runner_keywords.c" \
   "$root/src/runner_apps.c" \
   -DRUNNER_DEFAULT_DATA_DIRS='"tests/fixtures/runner-discovery/fallback-local:tests/fixtures/runner-discovery/fallback-system"' \
   -o "$test_binary"
"$test_binary"

python3 - "$root/src/makefile.py" <<'PY'
import runpy
import sys

makefile = runpy.run_path(sys.argv[1])
entry = makefile["desktop_entry_text"]()
for line in (
    "[Desktop Entry]",
    "Type=Application",
    "Name=GnuChan Runner",
    "Exec=/usr/local/bin/GnuChanRunner",
    "Categories=Utility;",
):
    if line not in entry.splitlines():
        raise SystemExit(f"runner desktop entry is missing {line!r}")
PY
