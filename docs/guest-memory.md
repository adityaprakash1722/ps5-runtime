# Guest memory and the loaded execution fixture

`GuestMemory` connects the offline loader's bytes and segment descriptions to
checked access through guest virtual addresses. A **guest address** belongs to the
program being loaded. A **host pointer** addresses our own process. They are not
interchangeable, even when both are 64-bit numbers.

## Logical memory API

`make_guest_memory` uses the existing validated loader to create backing bytes.
The default 64 MiB total-span limit still applies; this is an implementation limit,
not an attempt to model all console RAM. Gaps occupy zeroed space in this backing
array but do not become accessible guest segments.

`read`, `write`, and `check_execute` check all touched segments for ELF permission
bits (read=4, write=2, execute=1). Adjacent segments can be crossed only when each
permits the requested access. A gap is rejected. Address wraparound is rejected
before indexing. A zero-length operation is an explicit no-op: it does not prove
that the supplied address is mapped or permitted.

The complete range is checked before copying. Thus a failed write changes no
bytes, and a failed read leaves its destination unchanged. This is all-or-nothing
validation for a single API call, **not atomic synchronization between threads**.
There are no concurrent-access guarantees. The methods return an optional
diagnostic: no diagnostic means success. Diagnostics distinguish unmapped ranges,
permission denial, and arithmetic overflow.

The class does not expose mutable raw backing pointers. It also does not intercept
loads and stores made directly by native instructions. The native experiment has
separate mapped pages and explicitly copies the known output range back into this
logical model. That is not automatic write tracking or a coherent shared memory
implementation. General execution will need a deliberate unified memory design.

## One ELF through the pipeline

The `loaded-elf` fixture constructs an original ELF64 ET_DYN byte array with two
PT_LOAD segments, then actually passes it through:

```text
synthetic ELF bytes -> parse_elf -> make_guest_memory -> native page mapping
                   -> validated entry offset -> execute -> checked result/writeback
```

The first segment is readable/executable code. The second is readable/writable
data, located two host pages later in the guest layout. Its 32 file bytes occupy
40 memory bytes, so the last eight bytes start at zero (BSS-like storage).
There is an inaccessible host page between the segments, plus outer guard pages.
The `loaded-gap` fixture verifies that reading the protected middle page faults.

Native addresses are computed as `mapping_base + (guest_address - guest_base)`.
The fixture's code uses **RIP-relative addressing**: each reference is an offset
from the end of the current instruction, rather than an absolute address. Keeping
the code/data distance unchanged allows both to move together. This does not
implement relocation records or fix arbitrary absolute guest pointers.

The loaded code calculates `(5 + 9) * 3`, stores 42, XORs it with `0x55`, and
returns 127. C++ verifies the inputs, stored value, return value, and zero tail;
Python checks the reported result independently. The fixture is produced in memory
and contains no supplied code. No external ELF execution interface is added here.

## Startup contract and limitations

For `loaded-elf`, the explicit contract is `host-leaf-function-v1`: an x86-64, no-argument leaf
function called on the host stack under the host calling convention. The normal
host call supplies a return address. There is no guest stack switch, argc/argv,
auxiliary vector, TLS, import resolver, syscall dispatcher, or console startup.
An arbitrary ELF entry point does not imply this contract and must not be called
this way. Successful synthetic execution is not a claim that platform binaries
now run. A separate [synthetic startup experiment](startup-stack.md) uses this
same loader path with generated argument-reading code and a dedicated stack.
Its contract is `synthetic-stack-v1`, also not console startup. Authorized supplied
material remains available for local research.

Host protections are page-granular. Padding inside a partially used code or data
page may be physically accessible even though the logical guest API rejects it.
This fixture uses known separated pages; it does not decide how overlapping
segment pages or conflicting permissions should behave for arbitrary images.
The OS picks the host base, and only this position-independent fixture is executed.

Run through the supervisor with
`ctest --test-dir build -C Debug -R native_probe -V`, or run the successful fixture
alone with `build/Debug/ps5rt_native_probe.exe loaded-elf` on Windows. The regular
`ps5rt inspect` and `ps5rt plan` commands remain non-executing.

See [the execution experiment](execution-experiment.md) for host memory/process
references and [ELF references](references.md) for the public format definitions.
