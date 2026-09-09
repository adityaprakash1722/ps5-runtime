# ps5-runtime

A private, experimental foundation for a PS5-to-PC runtime. **This version does
not execute PS5 programs or games.** It is not an official release or a claim of
compatibility, performance, or endorsement.

The first milestone is intentionally testable: inspect an executable's structure
and calculate how a small supported subset would be laid out in memory, without
running it. All committed test inputs are generated from original synthetic data.
No console SDK, game code, firmware, keys, or community emulator source is included.

## What works today

- Bounded inspection of ELF64 little-endian headers, program segments, and sections.
- Standard `DT_NEEDED` library-name extraction where the implemented mapping rules apply.
- Explicit warnings for unimplemented platform-specific metadata; numeric values
  are retained without guessing what they mean.
- Offline layout of standard x86-64 `ET_EXEC` and `ET_DYN` files with nonoverlapping
  loadable ranges. The library can copy file bytes into an ordinary byte array and
  zero-fill the remaining space. The CLI only reports the plan.
- Human-readable summaries, versioned JSON reports, and deterministic diagnostics.
- Synthetic positive, negative, boundary, truncation, and seeded-mutation tests.
- A separate [native execution experiment](docs/execution-experiment.md): fixed
  original instruction fixtures with arithmetic, memory, fault, and timeout tests.
  It now includes a generated ELF passed through the loader before execution.
  It does not accept arbitrary ELF inputs or implement the PS5 ABI.
- A [guest-memory API](docs/guest-memory.md) with checked address ranges,
  permissions, gap rejection, and all-or-nothing range validation for writes.
- A [synthetic guest-stack experiment](docs/startup-stack.md): argument setup,
  nested calls, integer-register restoration, and guard-fault checks. This is a
  test contract, not platform process startup.

An accepted inspection means only that the implemented checks passed. It is not
a complete ELF conformance check, a runnable-image result, or a PS5 compatibility test.
See [architecture and terminology](docs/architecture.md) and [roadmap](docs/roadmap.md).

## Build and test

Requirements: CMake 3.24+, a C++20 compiler, and Python 3.10+ for tests and sample
generation. No third-party C++ libraries or Python packages are needed. Windows
uses Visual Studio 2022 C++ Build Tools with a Windows SDK. Run from a Developer
PowerShell/command prompt where `cmake`, `ctest`, and `python` are available.

The native experiment requires Windows MSVC x64 or Linux x86-64. Use
`-DPS5RT_NATIVE_PROBE=OFF` for an inspection-only build on other targets.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel 2
ctest --test-dir build -C Debug --output-on-failure --no-tests=error
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --no-tests=error
python tools/make_fixture.py --output build/demo.elf
.\build\Debug\ps5rt.exe inspect build/demo.elf
.\build\Debug\ps5rt.exe plan build/demo.elf --json
```

The generator refuses to overwrite an existing file; use a new filename to run it
again. If Python discovery fails, configure with
`-DPython3_EXECUTABLE="<absolute path to python.exe>"`.

Linux (GCC or Clang):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
python3 tools/make_fixture.py --output build/demo.elf
./build/ps5rt inspect build/demo.elf --json
```

For compiler-assisted memory and undefined-behavior checks on GCC/Clang, configure
a separate Debug directory with `-DPS5RT_SANITIZERS=ON`. GitHub CI runs Windows and
Linux Debug/Release tests plus a Clang sanitizer job. No private inputs or reports
are uploaded as CI artifacts. Warnings are errors by default.

## CLI contract

```text
ps5rt inspect <file> [--json]
ps5rt plan <file> [--json]
ps5rt --help
ps5rt --version
```

`inspect` reads only; `plan` adds an offline layout calculation. Neither writes an
image, allocates executable host pages, loads a guest library, or runs guest code.

| Exit | Meaning |
| --- | --- |
| 0 | Requested inspection or offline plan succeeded within the implemented subset |
| 1 | Parser rejected the file, including unsupported format features |
| 2 | Invalid command-line usage |
| 3 | File access, size limit, or host resource failure |
| 4 | Inspection succeeded but the requested offline plan was rejected |

JSON schema version 1 always reports `execution_supported: false`. Addresses,
offsets, and sizes use hexadecimal strings to avoid loss of 64-bit precision in
JSON consumers. Small identifiers and counts are numbers. `accepted_subset` is
deliberately narrower than “valid ELF.” `unsupported` dependencies may contain
standard names already decoded, but that list must not be treated as complete.
ELF names are raw byte strings: non-ASCII and control bytes are escaped as
`\u00XX`, not interpreted as UTF-8. To recover the bytes after JSON decoding,
encode the resulting name as Latin-1. Human output also quotes untrusted names.
Usage errors print help rather than JSON. Reports omit the input path but can
contain names from the input; keep reports for private files local.

Default limits: 256 MiB input; 4,096 program headers; 16,384 section headers;
65,536 dynamic entries; 1 MiB string-table/aggregate decoded-string budget;
64 MiB contiguous offline image span, including gaps. These are conservative
milestone limits, not PS5 hardware limits. The C++ APIs expose configurable limits,
except the loader's current 4,096-program-header policy.

## What comes next

For repeatable read-only batch inspection, see the
[local validation workflow](docs/local-validation.md). Its reports must stay
outside Git and contain sensitive metadata; ordinary CI uses synthetic inputs only.

Fixed native execution and a generated ELF-to-execution path are implemented
with C++ and Python expected-result checks, including a separate guest stack under
an explicit synthetic contract. Next establish platform loading/startup and ABI
support against verified evidence. Graphics, audio, input, storage, scheduling,
game compatibility, and frame-pacing work are separate milestones—not features
implied by reading an ELF file. See the [evidence-gated roadmap](docs/roadmap.md).

The repository remains private. Public release, licensing, branding, and inclusion
of third-party material require separate decisions. Do not add supplied internal
material to Git history. Public specification references are in
[references](docs/references.md).
