"""Black-box CLI tests: independent synthetic fixture, no private inputs."""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

exe = str(Path(sys.argv[1]).resolve())
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def invoke(*args, expected=0, json_output=False):
    result = subprocess.run([exe, *map(str, args)], capture_output=True,
                            encoding="utf-8", timeout=15)
    check(result.returncode == expected,
          f"exit {result.returncode}, expected {expected}: {result.stdout} {result.stderr}")
    if json_output:
        obj = json.loads(result.stdout)
        check(obj["schema_version"] == 1, "schema version")
        check(obj["execution_supported"] is False, "must not claim execution")
        return obj
    return result.stdout


with tempfile.TemporaryDirectory(prefix="ps5rt-test-") as folder:
    root = Path(folder)
    data = bytearray(272)
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0,
                     b"\x7fELF\x02\x01\x01" + bytes(9), 2, 62, 1,
                     0x400100, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 256, 0x400100, 0, 16, 32, 256)
    data[256:] = bytes(range(16))
    valid = root / "sample with spaces \u03bb.elf"
    valid.write_bytes(data)
    obj = invoke("inspect", valid, "--json", json_output=True)
    check(obj["inspection_status"] == "accepted_subset", "valid inspection")
    check(obj["header"]["machine"] == 62, "machine")
    check(obj["dependencies"]["status"] == "no_dynamic_table", "no dependencies")
    obj = invoke("plan", valid, "--json", json_output=True)
    check(obj["plan_status"] == "offline_only", "offline status")
    check(obj["plan"]["image_size"] == "0x20", "memory span")
    check(obj["plan"]["regions"][0]["file_size"] == "0x10", "file size")
    check("not implemented" in invoke("inspect", valid), "human-readable disclaimer")
    check("Offline layout" in invoke("plan", valid), "human-readable plan")
    # Names can contain arbitrary bytes. JSON must remain parseable and preserve
    # those bytes without interpreting terminal control sequences or UTF-8.
    named = bytearray(data)
    named.extend(bytes(704 - len(named)))
    raw_name = b'name"\\\n\x1b\xff'
    names = b"\0" + raw_name + b"\0"
    named.extend(names)
    struct.pack_into("<Q", named, 40, 512)
    struct.pack_into("<HHH", named, 58, 64, 3, 2)
    struct.pack_into("<IIQQQQIIQQ", named, 576, 1, 1, 0, 0, 256, 16, 0, 0, 1, 0)
    struct.pack_into("<IIQQQQIIQQ", named, 640, 0, 3, 0, 0, 704, len(names), 0, 0, 1, 0)
    named_path = root / "names.elf"
    named_path.write_bytes(named)
    obj = invoke("inspect", named_path, "--json", json_output=True)
    check(obj["sections"][1]["name"].encode("latin-1") == raw_name, "byte-safe JSON names")
    check("Usage:" in invoke("--help"), "help")
    check("0.1.0" in invoke("--version"), "version")
    invoke(expected=2)
    invoke("unknown", valid, expected=2)
    invoke("inspect", valid, "--unknown", expected=2)
    obj = invoke("inspect", root / "missing", "--json", expected=3, json_output=True)
    check(obj["diagnostics"][0]["code"] == "IO_FILE", "missing file")
    invoke("inspect", root, "--json", expected=3, json_output=True)
    broken = root / "broken.elf"
    for content in (b"", data[:63], b"X" + data[1:]):
        broken.write_bytes(content)
        obj = invoke("inspect", broken, "--json", expected=1, json_output=True)
        check(obj["inspection_status"] == "rejected", "invalid status")
    vendor = bytearray(data)
    struct.pack_into("<H", vendor, 16, 0xfe10)
    broken.write_bytes(vendor)
    obj = invoke("inspect", broken, "--json", json_output=True)
    check(obj["dependencies"]["status"] == "unsupported", "vendor evidence boundary")
    obj = invoke("plan", broken, "--json", expected=4, json_output=True)
    check(obj["plan_status"] == "rejected", "vendor plan unsupported")
    huge = root / "huge.elf"
    with huge.open("wb") as stream:
        stream.seek(256 * 1024 * 1024)
        stream.write(b"\0")
    obj = invoke("inspect", huge, "--json", expected=3, json_output=True)
    check(obj["diagnostics"][0]["code"] == "IO_LIMIT", "bounded file allocation")

    generator = Path(__file__).resolve().parents[1] / "tools" / "make_fixture.py"
    generated = root / "generated.elf"
    result = subprocess.run([sys.executable, str(generator), "--output", str(generated)],
                            capture_output=True, timeout=15)
    check(result.returncode == 0, "sample generator")
    obj = invoke("plan", generated, "--json", json_output=True)
    check(len(obj["plan"]["regions"]) == 2, "sample has two loadable regions")
    original = generated.read_bytes()
    result = subprocess.run([sys.executable, str(generator), "--output", str(generated)],
                            capture_output=True, timeout=15)
    check(result.returncode != 0 and generated.read_bytes() == original, "generator refuses overwrite")

print(f"{checks} CLI checks passed")
