#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ps5rt {

enum class Severity { warning, error };

struct Diagnostic {
    Severity severity;
    std::string code;
    std::string message;
    std::optional<std::uint64_t> offset;
};

struct ParseLimits {
    std::uint64_t max_file_bytes = 256ULL * 1024 * 1024;
    std::uint64_t max_program_headers = 4096;
    std::uint64_t max_section_headers = 16384;
    std::uint64_t max_dynamic_entries = 65536;
    std::uint64_t max_string_bytes = 1024 * 1024;
};

struct ElfHeader {
    std::uint8_t os_abi = 0;
    std::uint8_t abi_version = 0;
    std::uint16_t type = 0;
    std::uint16_t machine = 0;
    std::uint32_t flags = 0;
    std::uint64_t entry = 0;
    std::uint64_t program_offset = 0;
    std::uint64_t section_offset = 0;
    std::uint16_t program_entry_size = 0;
    std::uint16_t program_count = 0;
    std::uint16_t section_entry_size = 0;
    std::uint16_t section_count = 0;
    std::uint16_t section_names_index = 0;
};

struct ProgramHeader {
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;
    std::uint64_t virtual_address = 0;
    std::uint64_t physical_address = 0;
    std::uint64_t file_size = 0;
    std::uint64_t memory_size = 0;
    std::uint64_t alignment = 0;
};

struct SectionHeader {
    std::string name;
    std::uint32_t name_offset = 0;
    std::uint32_t type = 0;
    std::uint64_t flags = 0;
    std::uint64_t address = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t link = 0;
    std::uint32_t info = 0;
    std::uint64_t alignment = 0;
    std::uint64_t entry_size = 0;
};

enum class DependencyStatus { no_dynamic_table, standard, unsupported };

struct ElfImage {
    ElfHeader header;
    std::vector<ProgramHeader> programs;
    std::vector<SectionHeader> sections;
    DependencyStatus dependency_status = DependencyStatus::no_dynamic_table;
    std::vector<std::string> needed_libraries;
};

struct ParseResult {
    std::optional<ElfImage> image;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept { return image.has_value(); }
};

// Generic ELF64 little-endian structural inspection only. This does not establish
// platform compatibility, map host executable pages, resolve imports, or run code.
[[nodiscard]] ParseResult parse_elf(std::span<const std::uint8_t> bytes,
                                  const ParseLimits& limits = {});

} // namespace ps5rt
