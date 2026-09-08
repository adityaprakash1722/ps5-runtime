# Evidence-gated roadmap

The target experience is stable, low-latency play with correct output. That is a
long-term acceptance criterion, not a property of the current code. We cannot
promise universal compatibility, superiority to community projects, or a delivery
date based on this foundation.

| Stage | Deliverable | Evidence required before expanding |
| --- | --- | --- |
| 0: current | ELF inspection and bounded offline layout | Synthetic expected-value and rejection tests; Windows/Linux builds; sanitizer checks |
| 1: controlled execution | A deliberately tiny synthetic program, with a defined host/guest boundary | Expected registers, memory, return path, and fault behavior; no game inputs |
| 2: platform loading | A documented executable subset, relocations, imports, startup, and TLS | Versioned format evidence and independent tests for each supported behavior |
| 3: essential services | Memory, threads, synchronization, clocks, files, and diagnostics | Small reference programs; error paths and concurrency behavior compared against a trusted reference |
| 4: graphics/audio/input | Minimal rendering and playback with observable synchronization | Known-image/audio results, command traces, precision checks, input latency measurements |
| 5: a narrow game target | One explicitly selected, authorized build and configuration | Repeatable scenes, correctness comparisons, crash recovery, long-running tests |
| 6: quality and breadth | Broader workloads and sustained smoothness | Regression matrix across game versions, host hardware, drivers, and settings |

These stages can require revisiting earlier assumptions. Stage 1 still needs an
explicit execution design: native execution, translation, or another controlled
mechanism. Similar CPU instruction sets are helpful but do not settle system
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

The next implementation decision is stage 1's execution boundary and reference
test design. It is not yet time to load an arbitrary game into the host process.
