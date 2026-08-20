#!/usr/bin/env bash
# CLI contract and argument edge cases.
set -euo pipefail
BIN=${1:-./build/proxchunk}

fail() { echo "FAIL: $*" >&2; exit 1; }

out=$("$BIN" 2>&1) || true
echo "$out" | grep -q Usage || fail "no-args should print usage"
"$BIN" >/dev/null 2>&1 || fail "no-args exit should be 0"

"$BIN" -h >/dev/null 2>&1 || fail "-h"
"$BIN" --help >/dev/null 2>&1 || fail "--help"

ver=$("$BIN" -v)
echo "$ver" | grep -q '^proxchunk ' || fail "-v"
test "$ver" = "$("$BIN" --version)" || fail "-v/--version mismatch"
test "$ver" = "proxchunk 1.0" || fail "version want 'proxchunk 1.0' got '$ver'"

help=$("$BIN" -h 2>&1)
echo "$help" | grep -q 'logical CPUs' || fail "help should say concurrent default is logical CPUs"
echo "$help" | grep -q -- '--repl' || fail "help should mention --repl"
echo "$help" | grep -- '--chunk-mb' | grep -qi 'instead' \
    || fail "chunk-mb should be opt-in size split, not default 8 MiB"
echo "$help" | grep -- '--refresh' | grep -qi 'default: off' \
    || fail "refresh should default off (one-shot proxy test)"

"$BIN" --not-a-flag http://example.com >/dev/null 2>&1 && fail "unknown option should fail"
"$BIN" -c >/dev/null 2>&1 && fail "-c missing arg"
"$BIN" -c 0 http://example.com >/dev/null 2>&1 && fail "concurrent 0"
"$BIN" -s 0 http://example.com >/dev/null 2>&1 && fail "chunk-mb 0"
"$BIN" -p 0 http://example.com >/dev/null 2>&1 && fail "proxies 0"
"$BIN" -r -1 http://example.com >/dev/null 2>&1 && fail "refresh -1"

echo "test_cli ok"
