# Evidence-gated roadmap

The target experience is stable, low-latency play with correct output. That is a
long-term acceptance criterion, not a property of the current code. We cannot
promise universal compatibility, superiority to community projects, or a delivery
date based on this foundation.

| Stage | Deliverable | Evidence required before expanding |
| --- | --- | --- |
| 0: implemented subset | ELF inspection and bounded offline layout | Synthetic expected-value and rejection tests; Windows/Linux builds; sanitizer checks |
| 1a: fixed native experiment | Original instruction fixtures in a worker process; independent expected-result checks | RAX, memory, return, fault, and timeout checks; no platform compatibility claim |
| 1b: implemented experiment | Checked logical guest memory and generated ELF-to-execution path | Host-leaf-function contract only; relative addressing, BSS, permissions, result/writeback and gap fault checks |
| 1c: implemented experiment | Synthetic guest stack, argument frame and nested call bridge | Pointer/layout boundaries, argument checksums, stack alignment, restored host integers/RSP, guard fault |
| 1d: platform startup | Justified platform entry/ABI contract | Independently justified startup state, stack behavior, fault diagnostics; supported local inputs |
| 2: platform loading | A documented executable subset, relocations, imports, startup, and TLS | Versioned format evidence and independent tests for each supported behavior |
| 3: essential services | Memory, threads, synchronization, clocks, files, and diagnostics | Small reference programs; error paths and concurrency behavior compared against a trusted reference |
| 4: graphics/audio/input | Minimal rendering and playback with observable synchronization | Known-image/audio results, command traces, precision checks, input latency measurements |
| 5: a narrow game target | One explicitly selected, authorized build and configuration | Repeatable scenes, correctness comparisons, crash recovery, long-running tests |
| 6: quality and breadth | Broader workloads and sustained smoothness | Regression matrix across game versions, host hardware, drivers, and settings |

These stages can require revisiting earlier assumptions. Stage 1a explores native
execution; it does not settle the final execution architecture or connect a
arbitrary guest ELF to host execution. Stage 1b connects only a generated fixture
with a known host-leaf-function contract. Stage 1c extends this to a separate stack
under our own synthetic contract, not an inferred PS5 one. Similar CPU instruction sets do not settle system
calls, memory behavior, exception handling, or the platform ABI. Unknown calls
must stop with a useful diagnostic or return a justified error—not fabricated
success. A **system call** is a request from a program to its operating-system
services, rather than an ordinary function wholly implemented inside that program.

## Turning “buttery smooth” into measurements

**Frame time** is the elapsed time between delivered frames. Average frame rate
can hide occasional stalls; measure the distribution, especially slow outliers,
and show a timeline. **Frame pacing** concerns how evenly frames are presented.
Measure input-to-display latency separately: consistent frames can still respond
late to input. Include shader compilation stalls, asset streaming, audio dropouts,
memory growth, and long-run stability.

Before setting thresholds, select a game build, representative scenes, target PC
specifications, display settings, and a reference capture. Record cold-start and
warmed-cache results separately. Where an equivalent PC release exists, agree
on matched settings and comparison criteria; where one does not, compare against
the console reference and an explicitly defined PC experience target. “Looks
good to us” and an FPS screenshot are not sufficient acceptance tests.

## Working principles

- Keep platform observations separate from hypotheses and implementation policy.
- Learn from community designs and failures without importing unreviewed code or
  making claims about another project's quality without reproducible evidence.
- Add one observable behavior and its tests at a time; prevent regressions before
  increasing supported surface area.
- Keep internal evidence and real inputs outside Git and CI. Public or shared
  reports must not expose private paths, symbols, titles, or captured data.
- Publish compatibility claims only with reproducible scope: build, settings,
  hardware, test sequence, known failures, and evidence—not a vague “supported.”

The next implementation decision is stage 1d's platform startup contract and
reference tests. See [the synthetic stack experiment](startup-stack.md) for the
implemented bridge and its limits. Authorized material may be analyzed and executed locally where
supported; missing loader/ABI behavior is a technical gap, not a permission gap.
