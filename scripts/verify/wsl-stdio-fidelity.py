"""Spike for GhosttyWin32#206: does wsl.exe's redirected stdio relay
bytes untouched in both directions?

Round-trips test patterns through `wsl.exe cat`: stdin crosses
Windows->Linux, stdout crosses Linux->Windows, so one pass exercises
both directions. Any mangling (CRLF translation, ESC interpretation,
NUL truncation, chunk reordering) shows up as a mismatch.
"""

import os
import subprocess
import sys

CASES = {
    "all-256-bytes": bytes(range(256)),
    "line-endings": b"a\nb\r\nc\rd\n\r\x00e",
    "vt-sequences": b"\x1b[31mred\x1b[0m\x1b]0;title\x07\x1b[?2004h\ttab",
    "nul-heavy": b"\x00" * 64 + b"x" + b"\x00" * 64,
    "random-1mb": os.urandom(1024 * 1024),
}

failures = 0
for name, payload in CASES.items():
    proc = subprocess.run(
        ["wsl.exe", "cat"],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    ok = proc.stdout == payload
    if not ok:
        failures += 1
        got = proc.stdout
        # First differing offset, for the report.
        limit = min(len(got), len(payload))
        diff = next(
            (i for i in range(limit) if got[i] != payload[i]),
            limit,
        )
        print(
            f"FAIL {name}: sent={len(payload)}B got={len(got)}B "
            f"first-diff@{diff} "
            f"sent[{diff}:{diff+8}]={payload[diff:diff+8]!r} "
            f"got[{diff}:{diff+8}]={got[diff:diff+8]!r}"
        )
    else:
        print(f"OK   {name}: {len(payload)}B round-tripped intact")

print("verdict:", "CLEAN" if failures == 0 else f"{failures} case(s) mangled")
sys.exit(1 if failures else 0)
