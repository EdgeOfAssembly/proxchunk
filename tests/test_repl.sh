#!/usr/bin/env bash
# proxchunk --repl builtins and pass-through.
set -euo pipefail
BIN=${1:-./build/proxchunk}
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

fail() { echo "FAIL: $*" >&2; exit 1; }

out=$(printf 'help\nexit\n' | "$BIN" --repl)
echo "$out" | grep -q '^> ' || fail "prompt"
echo "$out" | grep -q builtins || fail "help text"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

out=$(printf 'mkdir -p sub/dir\ncd sub\npwd\nexit\n' | "$BIN" --repl)
echo "$out" | grep -q "$tmp/sub" || fail "cd/pwd: $out"
test -d "$tmp/sub/dir" || fail "mkdir -p"

printf 'x\n' > "$tmp/sub/f.txt"
out=$(printf 'cd %s\nrm f.txt\nrmdir dir\npwd\nexit\n' "$tmp/sub" | "$BIN" --repl)
test ! -e "$tmp/sub/f.txt" || fail "rm file"
test ! -d "$tmp/sub/dir" || fail "rmdir"

# pass-through: -v
out=$(printf -- '-v\nexit\n' | "$BIN" --repl)
echo "$out" | grep -q 'proxchunk 1.0' || fail "-v via repl: $out"

echo "test_repl.sh ok"
