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

"$BIN" --not-a-flag http://example.com >/dev/null 2>&1 && fail "unknown option should fail"
"$BIN" -c >/dev/null 2>&1 && fail "-c missing arg"
"$BIN" -c 0 http://example.com >/dev/null 2>&1 && fail "concurrent 0"
"$BIN" -s 0 http://example.com >/dev/null 2>&1 && fail "chunk-mb 0"
"$BIN" -p 0 http://example.com >/dev/null 2>&1 && fail "proxies 0"

echo "test_cli ok"
