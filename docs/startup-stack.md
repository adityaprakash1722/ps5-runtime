# A separate stack for generated guest code

The `guest-stack` fixture extends the generated ELF experiment: it loads original
test instructions, constructs arguments on a separate stack, switches to that
stack, runs a nested function call, and returns to the host. Its contract is named
`synthetic-stack-v1`. This is our test interface, **not a discovered PS5 ABI**.
An ABI (application binary interface) defines rules compiled code must agree on,
including where arguments live and which CPU state survives a call.

## What a stack is

A stack is memory used for temporary values and function-call bookkeeping.
`RSP` is the x86-64 **stack pointer**: a CPU register holding the current stack
address. A register is storage inside the CPU, distinct from ordinary RAM.
On this architecture the stack grows toward smaller addresses. Pushing a 64-bit
value subtracts eight from RSP and stores the value at the resulting address.
Popping reads a value there and adds eight to RSP.

`CALL` saves the address of the next instruction on the stack before transferring
control to a function. That saved **return address** tells `RET` where to resume.
For our ordinary near calls, each return address occupies eight bytes. A nested
call adds another return address; those addresses are removed in reverse order.
The stack is not a separate CPU: we change the address in RSP, not the processor.

The **host** is our PC program; the **guest** here is the generated test code.
The host already has a stack allocated by its operating system. Keeping a guest
stack separate lets us choose its initial layout and check its boundaries, but
does not by itself supply console behavior or isolate the guest from the process.

## Exact initial layout

`build_startup_stack` creates an ordinary zeroed byte vector. It does not allocate
executable memory or change CPU registers. Its inputs are a base address, stack
size, and a list of argument byte strings. The worker separately allocates native
read/write, non-executable pages and copies these bytes into them.

An **address** identifies a byte in memory. A **pointer** stores an address.
`argc` is the argument count. `argv` is an array of pointers to argument strings,
not the strings themselves. Each string ends in a zero byte, called a NUL
terminator. An empty string therefore still occupies one byte. The pointer after
the last argument is NULL: an eight-byte zero pointer, not a string terminator.
Non-ASCII bytes are preserved without interpreting an encoding.

For two arguments, at guest entry the layout is:

```text
higher addresses
    "probe\0alpha\0"        argument bytes, ending at the allocation's end
    optional zero padding
    NULL                    argv[2]
    pointer to "alpha"      argv[1]
    pointer to "probe"      argv[0]       <- entry RSP + 16
    2                       argc          <- frame = entry RSP + 8
    return address          written by CALL <- entry RSP
    space for nested calls and temporary storage
lower addresses
```

Every argc/pointer slot is eight bytes in **little-endian** order: the
least-significant byte is stored first. The builder puts strings at the highest
addresses, then rounds the argument frame down to a multiple of 16. This is
**alignment**: an address satisfying a divisibility rule. Immediately before
calling the guest, RSP equals that 16-aligned frame address. CALL subtracts eight,
so guest-entry RSP has remainder eight when divided by 16. Before its nested call,
the fixture subtracts another eight for alignment. The helper consequently enters
with RSP 16 bytes below the guest's entry RSP, also with remainder eight.

This is deliberately a CALL-based experiment, not an operating-system process
entry layout. A platform entry point must not be assumed to receive a return
address or this argument structure merely because it is in an ELF file.

The builder checks these implementation limits before allocating its output:

- Base and size must be multiples of 16; size is 4 KiB through 1 MiB inclusive.
  KiB means 1,024 bytes; MiB means 1,048,576 bytes.
- Base plus size must fit an unsigned 64-bit address without wrapping to zero.
- At most 64 arguments and 16 KiB of combined strings including terminators.
- Embedded NUL bytes are rejected because they would terminate an argument early.
- The frame and strings must fit, with at least 512 bytes below the frame.

These numbers are test policies, not PS5 limits or a promise that 512 bytes suffice
for arbitrary code. The builder checks numeric ranges, not whether the operating
system mapped a supplied base. It allows zero as a base for offline layout tests.
For native execution the worker supplies the actual allocation address, so the
argument pointers are directly usable host addresses. This special choice does
not implement general guest-address translation or relocations (adjustments to
stored addresses when moving a program). The stack is not yet part of `GuestMemory`.

## Crossing the host/guest boundary

A small generated **bridge** adapts the host function call to this test contract.
On Windows its three pointer arguments arrive in RCX, RDX and R8; on Linux x86-64
they arrive in RDI, RSI and RDX. It saves the host integer registers RBX, RBP, RDI,
RSI and R12 through R15 on the host stack. A **nonvolatile** register is one whose
value the caller expects a callee to preserve. This bridge saves the union of the
relevant host integer register sets, including two that Linux does not require.
The host-side conventions are documented by
[Microsoft](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)
and the [System V x86-64 ABI project](https://gitlab.com/x86-psABIs/x86-64-ABI).

The bridge then keeps the saved host-stack address in R12, switches RSP to the new
frame and calls the loaded entry. Our synthetic guest must leave R12 unchanged.
RDX supplies a separate observation-record pointer for recording test results;
it is instrumentation, not a claimed platform entry argument. The fixture changes
the other saved registers deliberately. When it returns, the bridge restores the
host RSP and saved integers, leaving the checksum return value in RAX. Neither the
guest nor its helper calls a host library while on the guest stack.

This is not a complete CPU-context switch. Floating-point and vector state are
not saved; the generated code does not change those registers or control settings.
Vector registers can hold multiple numeric values operated on by one instruction.
The bridge supplies no general exception-unwind metadata: information a runtime
uses to reconstruct prior function frames when handling an exception. It does not
register an OS-managed thread/fiber stack or implement sanitizer stack-switch
notifications. Only fixed, uninstrumented machine code runs on the alternate
stack; compiled C++ resumes on the host stack. Instrumentation means extra checks
inserted by a compiler. Passing the host sanitizer suite does not instrument or
validate the generated instructions themselves.

## What the tests establish

Six native cases cover zero arguments, empty strings, multiple strings, bytes
above 127, and a longer string. The guest helper adds all unsigned argument bytes
to argc. C++ and Python calculate expected checksums independently. Checks also
compare observed stack addresses/alignment, all eight saved integer registers,
restored host RSP, unchanged argument storage, and untouched low-stack sentinels.
Sentinels are known bytes used to detect unexpected changes. Separate core tests
decode every pointer and byte, and check exact limits, rejections and repeatability.

The native stack has 16 accessible host pages and an inaccessible page at each
end. These **guard pages** fault on access. They are permanent no-access mappings,
not Windows' one-shot `PAGE_GUARD` mechanism. `guest-stack-guard` deliberately reads
the lower guard while executing on the guest stack. The supervisor verifies the
specific access-violation exit on Windows or segmentation-fault signal on Linux.
It does not accept an arbitrary error exit as a pass. This proves that guard
access faults under that condition, not recovery from recursive stack exhaustion.
The worker dies; there is no resumption, automatic stack growth or in-process
exception recovery. A separate supervisor remains alive and runs further tests.

Run all native tests with `ctest --test-dir build -C Debug -R native_probe -V`.
On Windows, the successful case alone is
`build/Debug/ps5rt_native_probe.exe guest-stack`. Use the supervisor for fault cases.

Still absent: justified platform entry state; environment strings (`envp`);
an auxiliary vector (startup key/value metadata); thread-local storage (TLS,
per-thread variables); imported library resolution; system-call services; and
game graphics, audio and input. The next step is to establish and test a supported
platform startup/loading contract, rather than apply this synthetic one to
arbitrary supplied binaries. CLI inspection and planning remain non-executing.
