# Public specification references

The implementation is original code informed by the public ELF generic ABI.
These are references for standard format semantics, not PS5 format documentation:

- [ELF header](https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.eheader.html): identification fields, ELF class, byte order, type, and table locations.
- [Program header](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html): loadable segments, file and memory sizes, addresses, permissions, and alignment.
- [Sections](https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.sheader.html): section tables, names, types, and file-backed versus NOBITS storage.
- [Dynamic linking](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.dynamic.html): dynamic entries and standard dependency/string-table metadata.

The parser intentionally supports less than the complete specification. Numeric
OS-specific values do not establish their meaning. No private documentation,
third-party emulator implementation, or console code is reproduced in this repo.

Host memory, CPU, ABI, and process references for the separate execution probe
are listed in [execution experiment](execution-experiment.md#references).
