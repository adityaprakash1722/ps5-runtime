"""Supervisor/oracle for fixed native fixtures. Never loads corpus files."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

exe = str(Path(sys.argv[1]).resolve())
environment = dict(os.environ)
# Let deliberate fault fixtures reach the OS's default terminating handlers.
# Keep sanitizer instrumentation for all compiled host code, including arithmetic.
environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":handle_segv=0:handle_sigill=0:handle_sigbus=0"
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def ready(stdout, mode):
    lines = stdout.splitlines()
    check(bool(lines), "worker must report successful setup before a fault")
    start = json.loads(lines[0])
    check(start == {"phase": "ready", "fixture": mode, "external_binary_execution": False}, "ready protocol")
    return lines


def run(mode, timeout=15):
    return subprocess.run([exe, mode], capture_output=True, env=environment, timeout=timeout)


result = run("arithmetic")
check(result.returncode == 0 and not result.stderr, f"arithmetic failed: {result.returncode} {result.stderr!r}")
lines = ready(result.stdout, "arithmetic")
check(len(lines) == 2, "one complete result after ready")
report = json.loads(lines[1])
check(report["phase"] == "result" and report["fixture"] == "arithmetic", "result protocol")
check(report["ps5_execution_supported"] is False, "no PS5 execution claim")
check(report["memory_checks"] is True, "whole writable-page check")
check(report["case_count"] == len(report["cases"]) == 262, "all expected vectors executed")
mask64 = (1 << 64) - 1
for case in report["cases"]:
    # Independent arbitrary-precision Python oracle, with explicit 64-bit wrap.
    a, b, mask = (int(case[name], 16) for name in ("a", "b", "mask"))
    expected = ((a + b) * 3) & mask64
    check(int(case["stored"], 16) == expected, "stored product")
    check(int(case["returned"], 16) == (expected ^ mask), "RAX return value")

result = run("loaded-elf")
check(result.returncode == 0 and not result.stderr, f"loaded ELF failed: {result.returncode} {result.stderr!r}")
lines = ready(result.stdout, "loaded-elf")
check(len(lines) == 2, "loaded ELF returns one result")
loaded = json.loads(lines[1])
check(loaded["returned"] == ((5 + 9) * 3) ^ 0x55, "loaded RIP-relative code result")
check(loaded["stored"] == (5 + 9) * 3 and loaded["bss_zero"] is True, "loaded data and BSS")
check(loaded["contract"] == "host-leaf-function-v1" and loaded["ps5_execution_supported"] is False, "explicit startup scope")

for mode in ("illegal-instruction", "write-code", "guard-read", "non-executable", "loaded-gap"):
    result = run(mode)
    check(len(ready(result.stdout, mode)) == 1, "fault cannot produce success result")
    if os.name == "nt":
        expected = 0xC000001D if mode == "illegal-instruction" else 0xC0000005
        check(result.returncode & 0xFFFFFFFF == expected, f"unexpected Windows exception for {mode}: {result.returncode}")
    else:
        expected = signal.SIGILL if mode == "illegal-instruction" else signal.SIGSEGV
        check(result.returncode == -expected, f"unexpected signal for {mode}: {result.returncode} {result.stderr!r}")

try:
    run("hang", timeout=1)
    raise AssertionError("infinite-loop fixture did not time out")
except subprocess.TimeoutExpired as error:
    # subprocess.run kills and waits for this direct child before raising.
    ready(error.stdout or b"", "hang")
    check(True, "hung worker terminated and reaped")

result = run("not-a-fixture.elf")
check(result.returncode == 2 and not result.stdout, "arbitrary file arguments are not an execution interface")
# An earlier child failure must not prevent another worker from completing.
result = run("arithmetic")
check(result.returncode == 0 and len(ready(result.stdout, "arithmetic")) == 2, "supervisor survives child failures")
print(f"Native execution: 262 arithmetic vectors, loaded ELF, 5 expected faults, timeout recovery; {checks} checks passed")
