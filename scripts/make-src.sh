#!/usr/bin/env bash
# Source tarball from git (export-ignore strips host-only files).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${VER:-1.0}
cd "$ROOT"
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "working tree dirty — commit before building the source tarball" >&2
    exit 1
fi
mkdir -p dist
git archive --format=tar --prefix="proxchunk-${VER}/" HEAD \
    | gzip -9 > "dist/proxchunk-${VER}-src.tar.gz"
echo "wrote dist/proxchunk-${VER}-src.tar.gz"
tar -tzf "dist/proxchunk-${VER}-src.tar.gz"
ls -l "dist/proxchunk-${VER}-src.tar.gz"
