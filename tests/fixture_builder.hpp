#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

// These inputs are invented for the tests. They contain no console, game, SDK,
// or third-party emulator data. Nothing in this file runs the encoded bytes.
namespace fixture {

using Bytes = std::vector<std::uint8_t>;
inline constexpr std::size_t elf_header_size = 64;
inline constexpr std::size_t program_header_size = 56;
inline constexpr std::size_t section_header_size = 64;
inline constexpr std::size_t data_offset = 0x100;
inline constexpr std::uint64_t base_address = 0x1000;

inline void put(Bytes& bytes, std::size_t offset, std::uint64_t value,
                std::size_t width) {
    if (width > 8 || offset > bytes.size() || width > bytes.size() - offset) {
        throw std::out_of_range("synthetic fixture write is outside its buffer");
    }
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

inline void u16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    put(bytes, offset, value, 2);
}
inline void u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    put(bytes, offset, value, 4);
}
inline void u64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    put(bytes, offset, value, 8);
}

inline constexpr std::size_t ph(std::size_t index) {
    return elf_header_size + index * program_header_size;
}

inline void program(Bytes& bytes, std::size_t index, std::uint32_t type,
                    std::uint32_t flags, std::uint64_t offset,
                    std::uint64_t address, std::uint64_t file_size,
                    std::uint64_t memory_size, std::uint64_t alignment) {
    const auto start = ph(index);
    u32(bytes, start, type);
    u32(bytes, start + 4, flags);
    u64(bytes, start + 8, offset);
    u64(bytes, start + 16, address);
    u64(bytes, start + 24, address);
    u64(bytes, start + 32, file_size);
    u64(bytes, start + 40, memory_size);
    u64(bytes, start + 48, alignment);
}

inline Bytes minimal() {
    Bytes bytes(data_offset + 16, 0);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 2; // ELFCLASS64
    bytes[5] = 1; // ELFDATA2LSB
    bytes[6] = 1; // EV_CURRENT
    u16(bytes, 16, 2); // ET_EXEC
    u16(bytes, 18, 62); // EM_X86_64
    u32(bytes, 20, 1);
    u64(bytes, 24, base_address);
    u64(bytes, 32, elf_header_size);
    u16(bytes, 52, elf_header_size);
    u16(bytes, 54, program_header_size);
    u16(bytes, 56, 1);
    // No section table is required for this loadable synthetic image.
    program(bytes, 0, 1, 5, data_offset, base_address, 16, 32, 0x100);
    for (std::size_t index = 0; index < 16; ++index) {
        bytes[data_offset + index] = static_cast<std::uint8_t>(0xd0 + index);
    }
    return bytes;
}

inline Bytes two_loads() {
    auto bytes = minimal();
    bytes.resize(0x208, 0);
    u16(bytes, 56, 2);
    program(bytes, 1, 1, 6, 0x200, 0x1200, 8, 16, 0x100);
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[0x200 + index] = static_cast<std::uint8_t>(0xa0 + index);
    }
    return bytes;
}

inline constexpr std::size_t section_table_offset = 0x180;
inline constexpr std::size_t names_offset = 0x300;
inline constexpr std::size_t sh(std::size_t index) {
    return section_table_offset + index * section_header_size;
}

inline void section(Bytes& bytes, std::size_t index, std::uint32_t name,
                    std::uint32_t type, std::uint64_t flags,
                    std::uint64_t address, std::uint64_t offset,
                    std::uint64_t size, std::uint64_t alignment) {
    const auto start = sh(index);
    u32(bytes, start, name);
    u32(bytes, start + 4, type);
    u64(bytes, start + 8, flags);
    u64(bytes, start + 16, address);
    u64(bytes, start + 24, offset);
    u64(bytes, start + 32, size);
    u64(bytes, start + 48, alignment);
}

inline Bytes with_sections() {
    auto bytes = minimal();
    constexpr char names[] = "\0.text\0.bss\0.shstrtab\0";
    // Exclude the C++ literal's extra terminating NUL.
    constexpr auto names_size = sizeof(names) - 1;
    bytes.resize(names_offset + names_size, 0);
    u64(bytes, 40, section_table_offset);
    u16(bytes, 58, section_header_size);
    u16(bytes, 60, 4);
    u16(bytes, 62, 3);
    section(bytes, 1, 1, 1, 6, base_address, data_offset, 16, 16);
    // SHT_NOBITS occupies memory but has no bytes in the file at sh_offset.
    section(bytes, 2, 7, 8, 3, base_address + 16,
            std::numeric_limits<std::uint64_t>::max(), 32, 16);
    section(bytes, 3, 12, 3, 0, 0, names_offset, names_size, 1);
    for (std::size_t index = 0; index < names_size; ++index) {
        bytes[names_offset + index] = static_cast<std::uint8_t>(names[index]);
    }
    return bytes;
}

inline constexpr std::size_t dynamic_offset = 0x120;
inline constexpr std::size_t dynamic_strings_offset = 0x1c0;
inline constexpr std::uint64_t dynamic_strings_address = 0x10c0;
inline constexpr char library_name[] = "libsample.so";

inline void dynamic_entry(Bytes& bytes, std::size_t index,
                          std::uint64_t tag, std::uint64_t value) {
    u64(bytes, dynamic_offset + index * 16, tag);
    u64(bytes, dynamic_offset + index * 16 + 8, value);
}

inline Bytes with_dynamic() {
    auto bytes = minimal();
    bytes.resize(0x200, 0);
    u16(bytes, 56, 2);
    program(bytes, 0, 1, 5, data_offset, base_address, 0x100, 0x120, 0x100);
    program(bytes, 1, 2, 4, dynamic_offset, 0x1020, 64, 64, 8);
    constexpr auto strings_size = sizeof(library_name) + 1;
    dynamic_entry(bytes, 0, 5, dynamic_strings_address); // DT_STRTAB
    dynamic_entry(bytes, 1, 10, strings_size); // DT_STRSZ
    dynamic_entry(bytes, 2, 1, 1); // DT_NEEDED, skip leading empty string
    dynamic_entry(bytes, 3, 0, 0); // DT_NULL
    for (std::size_t index = 0; index < sizeof(library_name); ++index) {
        bytes[dynamic_strings_offset + 1 + index] =
            static_cast<std::uint8_t>(library_name[index]);
    }
    return bytes;
}

} // namespace fixture
