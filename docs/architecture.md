# Architecture and concepts

## What an executable describes

An executable file is more than processor instructions. It also describes where
code and data belong, what other components they need, and where startup begins.
**ELF** (Executable and Linkable Format) is one format for that information. This
project currently reads only its 64-bit, little-endian form. Little-endian means
the least significant byte of a number is stored first: bytes `34 12` represent
hexadecimal `0x1234`, not `0x3412`.

The initial **header** identifies the file and gives the locations and sizes of
other tables. We decode fields from individual bytes after checking bounds. We
do not cast untrusted bytes to a C++ structure: structure padding, alignment, and
the host's byte order are not a reliable description of an on-disk format.

A **program header** describes a segment, a range relevant to loading the program.
`PT_LOAD` says that certain file bytes belong at a specified virtual address.
A **section header** describes a section, a grouping used by tools for things
such as code, strings, or debugging information. Sections and segments are not
interchangeable: a segment can cover multiple sections, and an executable can
have no section table. Our loader therefore uses program headers.

A **file offset** counts bytes from the beginning of the file. A **virtual
address** is an address in the program's own address space. It is not necessarily
the same as either a file offset or an address in our Windows/Linux process.
This milestone never tries to access a guest virtual address as a host pointer.

## Implemented pipeline

1. The CLI reads a bounded regular file into a byte array.
2. `parse_elf` checks the supported structure and returns decoded metadata plus
   diagnostics. A diagnostic has severity, a stable category code, an explanation,
   and optionally a file offset. Unknown platform semantics remain unknown.
3. `make_load_plan` checks the narrower offline-loading policy, sorts nonempty
   loadable ranges, and computes their enclosing span.
4. The library's `materialize_image` rechecks that plan, creates a zeroed byte
   array, and copies each segment's file-backed bytes into it. Tests exercise this
   operation; the CLI exposes only steps 1–3.

For example, a segment may store 16 bytes in the file but require 32 bytes in
memory. We copy the 16 bytes and leave the remaining 16 zero. This supports the
basic zero-initialized storage behavior often associated with **BSS**: storage
reserved for initially zero variables without storing all those zeros on disk.
Gaps between segments are also zero in our offline array. That does **not** mean
such gaps should become accessible pages in an eventual executable mapping.

The array index for a guest address is `guest_address - plan.base_address`.
For example, base `0x400000` and address `0x400010` yield array index `0x10` (16).
The array contains a representation of the layout, not runnable guest memory.

## Why layout is not execution

**ET_EXEC** and **ET_DYN** are standard ELF type identifiers for executable and
shared-object forms. ET_DYN often participates in position-independent loading.
Our ET_DYN plan retains the original addresses with zero added load bias. A
**load bias** is the amount added when placing an image at a different base.

**Relocations** tell a loader how to adjust addresses or references after final
placement and symbol resolution. A **symbol** is a named entity such as a
function or variable. An **import** refers to an entity supplied by another
component. **DT_NEEDED** entries name required libraries in the standard dynamic
table, but listing those names does not resolve any imported functions. Neither
relocation nor import resolution is implemented here.

**TLS** (thread-local storage) gives each thread its own instance of selected
variables. A **thread** is a schedulable sequence of execution within a process;
multiple threads can share ordinary memory. TLS setup, thread startup, stacks,
exceptions, and scheduling are not implemented. An eventual **mutex** (mutual
exclusion lock) lets one thread at a time enter a protected operation; another
thread trying to acquire the same lock must wait or receive an appropriate
failure. Correct emulation must preserve observable lock and wake-up behavior,
not merely provide a function with the same name.

Memory **permissions** determine whether a page can be read, written, or executed.
The loader records ELF flags but does not implement host page permissions. It
also does not establish an **ABI** (application binary interface): the rules for
passing function arguments, returning results, organizing data, and interacting
with the operating system. Sharing a CPU instruction set with a PC does not
make a console binary a PC application.

## Deliberate restrictions

Inspection is not a complete validator of every ELF relationship. It does not
check instruction semantics, all section links, relocations, symbols, or every
dynamic tag. Extended table numbering is explicitly rejected in version 0.1.
Unknown types and platform fields are preserved with conservative diagnostics.
Vendor-specific dependency information is not guessed from standard fields.

Offline loading requires machine identifier 62 (x86-64), standard ET_EXEC/ET_DYN,
nonoverlapping nonempty PT_LOAD byte ranges, supported segment flags, bounded
file ranges and addresses, and a bounded total span. A nonzero entry address must
lie in file-backed bytes of an executable segment. An entry of zero is allowed
with a warning. Some legitimate ELF files fall outside this restricted policy;
rejection is not proof that those files are corrupt.

All additions and ranges are checked before indexing or allocation. For example,
`size <= file_length - offset` is safe only after checking `offset <= file_length`.
Blindly checking `offset + size <= file_length` can fail when unsigned addition
wraps around its maximum value. Resource limits also prevent tiny metadata from
requesting enormous allocations. Limits are configurable policy, not hardware
facts. Allocation failures can still occur; the CLI reports host resource errors.

## Testing and evidence

A **test oracle** is the independently justified expected answer for a test.
Here, synthetic fixtures have small, explicit headers and known byte patterns;
tests compare decoded values, rejected cases, copied bytes, and zeroed ranges.
They do not derive every expected value by asking the implementation itself.
Additional seeded mutations check repeatability and result invariants. These
small mutation checks are not exhaustive fuzzing or proof of memory safety.

Tests stay active in Release builds. CI also uses **AddressSanitizer** to detect
many invalid memory accesses and **UndefinedBehaviorSanitizer** to catch several
classes of operations that C++ does not define. Passing these checks improves
confidence for exercised cases; it does not prove universal correctness.

Real platform behavior needs a stronger oracle: versioned documentation and/or
repeatable observations on authorized reference hardware. Each future test must
record what it establishes and what remains untested. A parser success must
never be rebranded as a game-boot result.
