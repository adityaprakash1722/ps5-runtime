# Project instructions

This is a private, experimental PS5-to-PC runtime project. The current milestone
includes ELF inspection, offline loading, and controlled execution experiments;
game execution is not implemented.

- Use C++20 for the core, CMake/CTest for builds/tests, and Python standard library
  only for fixture generation and command-line integration tests.
- Authorized supplied material may be analyzed and executed locally as supported
  by the implementation. Do not treat local execution as a permission blocker.
  Do not bypass missing loader/ABI behavior or describe unsupported execution as
  success. Keep internal material out of commits and CI; committed tests remain
  synthetic. Preserve local evidence separately from published implementation.
- Do not copy source from external emulator projects into this repository.
- Never turn an unsupported operation into reported success. Use stable diagnostic
  codes and keep structural validity separate from runtime support.
- Parsing must use bounded little-endian reads, checked arithmetic, explicit
  resource limits, and deterministic diagnostics. Avoid packed-struct casts.
- Test negative paths and known-bad cases; no assertions compiled out in Release.
- Preserve evidence boundaries in documentation. A synthetic test is not proof of
  PS5 compatibility. Do not invent platform constants or unmeasured performance.
- No source material, private paths, titles, symbols, dumps, local logs, credentials,
  or captures in GitHub issues, workflows, build artifacts, or repository history.
- Keep the public-format references in docs/references.md. Read local code before
  changing an interface, and run the relevant tests before committing.
