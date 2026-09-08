"""Generate an original, synthetic ELF file. It is not a console program."""
import argparse
import struct
from pathlib import Path


def fixture():
    data = bytearray(0x204)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, ident, 2, 62, 1,
                     0x400000, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0x100, 0x400000, 0, 4, 8, 0x100)
    struct.pack_into("<IIQQQQQQ", data, 120, 1, 6, 0x200, 0x401000, 0, 4, 16, 0x100)
    data[0x100:0x104] = b"TEST"
    data[0x200:0x204] = b"DATA"
    return data


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(fixture())
    print("Synthetic fixture created (existing files are never overwritten).")
