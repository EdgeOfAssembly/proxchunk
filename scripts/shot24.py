#!/usr/bin/env python3
"""24 Hz screenshots via xmux ctl keep-alive (one X connection)."""

from __future__ import annotations

import argparse
import subprocess
import sys
import threading
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="24 screenshots/sec via xmux ctl")
    parser.add_argument("session", nargs="?", default="proxchunk")
    parser.add_argument("-o", "--output", default="/tmp/proxchunk/shots/fps/f.png")
    parser.add_argument("-d", "--duration", type=float, default=90.0)
    parser.add_argument("-r", "--rate", type=float, default=24.0)
    args = parser.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    interval = 1.0 / args.rate

    proc = subprocess.Popen(
        ["xmux", "ctl", args.session],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert proc.stdin is not None
    assert proc.stdout is not None

    n_ok = 0
    n_err = 0

    def drain() -> None:
        nonlocal n_ok, n_err
        assert proc.stdout is not None
        for raw in proc.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip()
            if line.startswith("OK"):
                n_ok += 1
            elif line.startswith("ERR"):
                n_err += 1

    threading.Thread(target=drain, daemon=True).start()

    t0 = time.monotonic()
    n_sent = 0
    next_t = t0
    try:
        while time.monotonic() - t0 < args.duration:
            proc.stdin.write(f"SHOT {out}\n".encode())
            proc.stdin.flush()
            n_sent += 1
            next_t += interval
            delay = next_t - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        proc.stdin.write(b"QUIT\n")
        proc.stdin.flush()
    except BrokenPipeError:
        pass

    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    elapsed = max(time.monotonic() - t0, 1e-6)
    print(
        f"shot24: sent={n_sent} ok={n_ok} err={n_err} "
        f"elapsed={elapsed:.2f}s rate={n_ok / elapsed:.2f}/s",
        file=sys.stderr,
    )
    return 0 if n_ok > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
