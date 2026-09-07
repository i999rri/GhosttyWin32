"""Verification for GhosttyWin32#206 step 1: the WSL-side helper.

Drives ghostty-wsl-helper through `wsl.exe` exactly the way BridgePty
will: framed stdin (data/resize), raw stdout. Checks that

1. the child runs on a real pty with the initial size from --cols/--rows,
2. data frames reach the child and its output comes back raw,
3. a resize frame lands as TIOCSWINSZ (child sees the new size),
4. --term overrides TERM in the child environment,
5. the helper exits with the child's exit code, and frames split
   across writes still parse,
6. a hangup frame ends the session (SIGHUP to the child), and
7. killing wsl.exe does not leave a helper behind in the distro.
"""

import re
import struct
import subprocess
import sys
import time

HELPER = r"external/ghostty/zig-out/bin/ghostty-wsl-helper"


def wsl_path(win_path: str) -> str:
    out = subprocess.run(
        ["wsl.exe", "wslpath", "-a", win_path.replace("\\", "/")],
        capture_output=True,
        text=True,
    )
    return out.stdout.strip()


def frame(kind: int, payload: bytes) -> bytes:
    return struct.pack("<BH", kind, len(payload)) + payload


def data(payload: bytes) -> bytes:
    return frame(0, payload)


def resize(cols: int, rows: int, xpx: int = 0, ypx: int = 0) -> bytes:
    return frame(1, struct.pack("<4H", cols, rows, xpx, ypx))


failures = 0


def check(name: str, ok: bool, detail: str = ""):
    global failures
    if not ok:
        failures += 1
    print(f"{'OK  ' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""))


helper = wsl_path(HELPER)

# 1+4: pty size and TERM, via a one-shot command.
out = subprocess.run(
    ["wsl.exe", helper, "--cols", "111", "--rows", "33",
     "--", "/bin/sh", "-c", "stty size; tty"],
    capture_output=True,
)
text = out.stdout.decode(errors="replace")
check("initial size via TIOCGWINSZ", "33 111" in text, text.strip())
check("child is on a real pts", "/dev/pts/" in text, text.strip())

# TERM is checked against env directly: a shell may legitimately rewrite
# it at startup (NixOS-WSL's wrapped /bin/sh does).
out = subprocess.run(
    ["wsl.exe", helper, "--term", "xterm-ghostty", "--", "/usr/bin/env"],
    capture_output=True,
)
check("--term overrides TERM", b"TERM=xterm-ghostty" in out.stdout,
      repr([l for l in out.stdout.splitlines() if b"TERM" in l]))

# 2+3+5: interactive session over frames.
proc = subprocess.Popen(
    ["wsl.exe", helper, "--cols", "80", "--rows", "24", "--", "/bin/sh"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
proc.stdin.write(data(b"stty -echo\n"))
proc.stdin.write(data(b"stty size\n"))
proc.stdin.flush()
time.sleep(1.0)
proc.stdin.write(resize(132, 50))
# Split one data frame across two writes to exercise the incremental parser.
second = data(b"stty size\nexit 42\n")
proc.stdin.write(second[:2])
proc.stdin.flush()
time.sleep(0.2)
proc.stdin.write(second[2:])
proc.stdin.flush()

stdout, stderr = proc.communicate(timeout=20)
sizes = re.findall(rb"(\d+) (\d+)", stdout)
check("data frame reaches child", b"24 80" in stdout, repr(stdout[:120]))
check("resize frame applies", b"50 132" in stdout, repr(stdout[:120]))
check("split frame parses", (b"24 80" in stdout) and (b"50 132" in stdout))
check("child exit code propagates", proc.returncode == 42, f"rc={proc.returncode} stderr={stderr!r}")

# 6: hangup frame hangs up an idle shell.
proc = subprocess.Popen(
    ["wsl.exe", helper, "--", "/bin/sh"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
)
time.sleep(1.0)
proc.stdin.write(frame(2, b""))
proc.stdin.flush()
try:
    proc.wait(timeout=10)
    check("hangup frame ends session", proc.returncode == 129,
          f"rc={proc.returncode}")
except subprocess.TimeoutExpired:
    proc.kill()
    check("hangup frame ends session", False, "helper did not exit")

# 7: killing wsl.exe must not leave the helper lingering in the distro.
proc = subprocess.Popen(
    ["wsl.exe", helper, "--", "/bin/sh"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
)
time.sleep(1.5)
proc.kill()
proc.wait()
time.sleep(1.5)
left = subprocess.run(
    ["wsl.exe", "pgrep", "-f", "ghostty-wsl-helper"],
    capture_output=True, text=True,
)
check("no helper lingers after wsl.exe dies", left.stdout.strip() == "",
      f"pids={left.stdout.strip()!r}")

print("verdict:", "CLEAN" if failures == 0 else f"{failures} check(s) failed")
sys.exit(1 if failures else 0)
