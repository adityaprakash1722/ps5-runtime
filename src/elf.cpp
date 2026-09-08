#include "ps5rt/elf.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace ps5rt {
namespace {

constexpr std::uint64_t elf_header_size = 64;
constexpr std::uint64_t program_header_size = 56;
constexpr std::uint64_t section_header_size = 64;
constexpr std::uint32_t pt_null = 0, pt_load = 1, pt_dynamic = 2;
constexpr std::uint32_t sht_null = 0, sht_strtab = 3, sht_nobits = 8;

bool add(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
    result = a + b;
    return true;
}

bool multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
    result = a * b;
    return true;
}

bool aligned_power(std::uint64_t n) { return n <= 1 || (n & (n - 1)) == 0; }

class Parser {
public:
    Parser(std::span<const std::uint8_t> bytes, const ParseLimits& limits)
        : bytes_(bytes), limits_(limits) {}

    ParseResult run() {
        if (bytes_.size() > limits_.max_file_bytes) {
            error("ELF_LIMIT", "File exceeds the configured file-byte limit.");
        } else if (header() && programs() && sections() && dependencies()) {
            result_.image = std::move(image_);
        }
        return std::move(result_);
    }

private:
    std::span<const std::uint8_t> bytes_;
    const ParseLimits& limits_;
    ParseResult result_;
    ElfImage image_;
    std::uint64_t copied_string_bytes_ = 0;
    bool dependency_extensions_ = false;

    bool error(std::string code, std::string message,
               std::optional<std::uint64_t> offset = {}) {
        result_.diagnostics.push_back({Severity::error, std::move(code), std::move(message), offset});
        return false;
    }

    void warning(std::string code, std::string message,
                 std::optional<std::uint64_t> offset = {}) {
        // One diagnostic per warning category bounds output for repetitive metadata.
        if (std::none_of(result_.diagnostics.begin(), result_.diagnostics.end(),
                         [&](const Diagnostic& d) { return d.code == code; })) {
            result_.diagnostics.push_back({Severity::warning, std::move(code), std::move(message), offset});
        }
    }

    bool range(std::uint64_t offset, std::uint64_t size) const {
        return offset <= bytes_.size() && size <= bytes_.size() - offset;
    }

    template<class T> bool read(std::uint64_t offset, T& value) const {
        if (!range(offset, sizeof(T))) return false;
        std::uint64_t decoded = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            decoded |= static_cast<std::uint64_t>(bytes_[static_cast<std::size_t>(offset) + i]) << (8 * i);
        }
        value = static_cast<T>(decoded);
        return true;
    }

    bool table(std::uint64_t offset, std::uint64_t count, std::uint64_t stride) {
        std::uint64_t size = 0;
        if (!multiply(count, stride, size) || (count != 0 && offset == 0) || !range(offset, size)) {
            return error("ELF_TABLE_BOUNDS", "Header table is outside the file or its size overflows.", offset);
        }
        return true;
    }

    bool header() {
        if (!range(0, elf_header_size)) return error("ELF_HEADER_TRUNCATED", "ELF64 requires a complete 64-byte header.", 0);
        if (bytes_[0] != 0x7f || bytes_[1] != 'E' || bytes_[2] != 'L' || bytes_[3] != 'F')
            return error("ELF_MAGIC", "Input does not have the ELF magic bytes.", 0);
        if (bytes_[4] != 2) return error("ELF_CLASS", "Only ELF64 is supported.", 4);
        if (bytes_[5] != 1) return error("ELF_ENDIAN", "Only little-endian ELF is supported.", 5);
        if (bytes_[6] != 1) return error("ELF_VERSION", "Unsupported ELF identification version.", 6);

        auto& h = image_.header;
        h.os_abi = bytes_[7];
        h.abi_version = bytes_[8];
        std::uint32_t version = 0;
        std::uint16_t header_size = 0;
        if (!read(16, h.type) || !read(18, h.machine) || !read(20, version) ||
            !read(24, h.entry) || !read(32, h.program_offset) || !read(40, h.section_offset) ||
            !read(48, h.flags) || !read(52, header_size) || !read(54, h.program_entry_size) ||
            !read(56, h.program_count) || !read(58, h.section_entry_size) ||
            !read(60, h.section_count) || !read(62, h.section_names_index))
            return error("ELF_HEADER_TRUNCATED", "Unable to read the ELF64 header.", 0);
        if (version != 1) return error("ELF_VERSION", "Unsupported ELF header version.", 20);
        if (header_size != elf_header_size) return error("ELF_HEADER_SIZE", "ELF64 header size must be 64 bytes.", 52);
        if (h.program_count == 0xffff || h.section_names_index == 0xffff ||
            (h.section_count == 0 && h.section_offset != 0))
            return error("ELF_EXTENDED_NUMBERING", "Extended ELF table numbering is explicitly unsupported in this version.", 56);
        if (h.section_count >= 0xff00)
            return error("ELF_EXTENDED_NUMBERING", "Reserved section counts require extended numbering, which is unsupported.", 60);
        if (h.program_count > limits_.max_program_headers || h.section_count > limits_.max_section_headers)
            return error("ELF_LIMIT", "ELF header count exceeds the configured limit.", 56);
        if (h.program_entry_size != program_header_size && (h.program_count != 0 || h.program_entry_size != 0))
            return error("ELF_PROGRAM_ENTRY_SIZE", "Program header entry size must be 56, or zero when the table is absent.", 54);
        if (h.section_entry_size != section_header_size && (h.section_count != 0 || h.section_entry_size != 0))
            return error("ELF_SECTION_ENTRY_SIZE", "Section header entry size must be 64, or zero when the table is absent.", 58);
        if ((h.section_count == 0 && h.section_names_index != 0) ||
            (h.section_names_index != 0 && h.section_names_index >= h.section_count))
            return error("ELF_SECTION_NAME", "Section-name table index does not identify a section.", 62);
        if (!table(h.program_offset, h.program_count, program_header_size) ||
            !table(h.section_offset, h.section_count, section_header_size)) return false;
        if (h.os_abi != 0 || h.abi_version != 0 || h.flags != 0)
            warning("ELF_PLATFORM_METADATA", "OS ABI, ABI version, and processor flags are preserved numerically; their platform semantics are not validated.", 7);
        if (h.machine != 62)
            warning("ELF_MACHINE_UNSUPPORTED", "Machine identifier is preserved; this project does not implement execution for this architecture.", 18);
        if (h.type > 4) {
            dependency_extensions_ = true;
            warning("ELF_TYPE_UNSUPPORTED", "Nonstandard ELF type is preserved; platform-specific loading and dependency semantics are unsupported.", 16);
        }
        return true;
    }

    bool programs() {
        const auto& h = image_.header;
        image_.programs.reserve(h.program_count);
        for (std::uint64_t i = 0; i < h.program_count; ++i) {
            const auto offset = h.program_offset + i * program_header_size; // table() bounded this arithmetic.
            ProgramHeader p;
            if (!read(offset, p.type) || !read(offset + 4, p.flags) || !read(offset + 8, p.offset) ||
                !read(offset + 16, p.virtual_address) || !read(offset + 24, p.physical_address) ||
                !read(offset + 32, p.file_size) || !read(offset + 40, p.memory_size) || !read(offset + 48, p.alignment))
                return error("ELF_TABLE_BOUNDS", "Truncated program header.", offset);
            image_.programs.push_back(p);
            if (p.type == pt_null) continue; // Other fields of PT_NULL are undefined.
            if (!range(p.offset, p.file_size))
                return error("ELF_SEGMENT_BOUNDS", "Program segment's file range is outside the input.", offset + 8);
            if (p.type == pt_load) {
                if (p.file_size > p.memory_size)
                    return error("ELF_SEGMENT_SIZE", "PT_LOAD file size exceeds its memory size.", offset + 32);
                std::uint64_t end = 0;
                if (!add(p.virtual_address, p.memory_size, end))
                    return error("ELF_SEGMENT_BOUNDS", "PT_LOAD virtual-address range overflows.", offset + 16);
                if (!aligned_power(p.alignment) || (p.alignment > 1 && p.virtual_address % p.alignment != p.offset % p.alignment))
                    return error("ELF_SEGMENT_ALIGNMENT", "PT_LOAD alignment must be zero, one, or a power of two with congruent file and virtual addresses.", offset + 48);
            }
            if (p.type > 7) {
                dependency_extensions_ = true;
                warning("ELF_PROGRAM_TYPE_UNSUPPORTED", "Nonstandard program-header types are preserved, but extension-specific loading and dependencies are not interpreted.", offset);
            }
        }
        return true;
    }

    bool string_at(std::uint64_t base, std::uint64_t size, std::uint64_t index,
                   std::string& value, const char* code) {
        if (index >= size || !range(base, size)) return error(code, "String index is outside its declared string table.", base);
        const auto available = size - index;
        const auto start = base + index; // Whole table range is already bounded.
        std::uint64_t length = 0;
        while (length < available && bytes_[static_cast<std::size_t>(start + length)] != 0) ++length;
        if (length == available) return error(code, "String has no NUL terminator within its declared table.", start);
        std::uint64_t cost = 0, total = 0;
        if (!add(length, 1, cost) || !add(copied_string_bytes_, cost, total) || total > limits_.max_string_bytes)
            return error("ELF_LIMIT", "Combined decoded strings exceed the configured string-byte limit.", start);
        copied_string_bytes_ = total;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + static_cast<std::size_t>(start)), static_cast<std::size_t>(length));
        return true;
    }

    bool sections() {
        const auto& h = image_.header;
        image_.sections.reserve(h.section_count);
        for (std::uint64_t i = 0; i < h.section_count; ++i) {
            const auto offset = h.section_offset + i * section_header_size;
            SectionHeader s;
            if (!read(offset, s.name_offset) || !read(offset + 4, s.type) || !read(offset + 8, s.flags) ||
                !read(offset + 16, s.address) || !read(offset + 24, s.offset) || !read(offset + 32, s.size) ||
                !read(offset + 40, s.link) || !read(offset + 44, s.info) || !read(offset + 48, s.alignment) || !read(offset + 56, s.entry_size))
                return error("ELF_TABLE_BOUNDS", "Truncated section header.", offset);
            image_.sections.push_back(std::move(s));
            const auto& section = image_.sections.back();
            if (i == 0 && section.type != sht_null)
                return error("ELF_SECTION_BOUNDS", "Section zero must have SHT_NULL type.", offset + 4);
            if (section.type == sht_null) continue; // Undefined fields are not payload ranges or names.
            if (section.type != sht_nobits && !range(section.offset, section.size))
                return error("ELF_SECTION_BOUNDS", "Section payload extends outside the input.", offset + 24);
            if (!aligned_power(section.alignment))
                return error("ELF_SECTION_ALIGNMENT", "Section alignment must be zero, one, or a power of two.", offset + 48);
            if (section.type >= 0x60000000U) {
                dependency_extensions_ = true;
                warning("ELF_SECTION_TYPE_UNSUPPORTED", "Extension-specific section types are preserved without interpreting their semantics.", offset + 4);
            }
        }
        if (h.section_names_index == 0) {
            for (const auto& section : image_.sections) {
                if (section.type != sht_null && section.name_offset != 0) {
                    warning("ELF_SECTION_NAMES_ABSENT", "Section name offsets cannot be interpreted because the file declares no section-name table.", h.section_offset);
                    break;
                }
            }
            return true;
        }
        const auto& names = image_.sections[h.section_names_index];
        if (names.type != sht_strtab)
            return error("ELF_SECTION_NAME", "Section-name table does not have SHT_STRTAB type.", h.section_offset + h.section_names_index * section_header_size + 4);
        if (names.size > limits_.max_string_bytes)
            return error("ELF_LIMIT", "Section-name string table exceeds the configured string-byte limit.", names.offset);
        if (names.size != 0 && (bytes_[static_cast<std::size_t>(names.offset)] != 0 || bytes_[static_cast<std::size_t>(names.offset + names.size - 1)] != 0))
            return error("ELF_SECTION_NAME", "A nonempty string table must begin and end with NUL bytes.", names.offset);
        for (auto& section : image_.sections) {
            if (section.type == sht_null) continue;
            if (section.name_offset == 0) continue; // Zero denotes no name, including an empty table.
            if (!string_at(names.offset, names.size, section.name_offset, section.name, "ELF_SECTION_NAME")) return false;
        }
        return true;
    }

    bool dependencies() {
        const ProgramHeader* dynamic = nullptr;
        for (const auto& p : image_.programs) {
            if (p.type != pt_dynamic) continue;
            if (dynamic) return error("ELF_DYNAMIC_AMBIGUOUS", "Multiple PT_DYNAMIC segments are unsupported.", p.offset);
            dynamic = &p;
        }
        if (!dynamic) {
            if (dependency_extensions_) image_.dependency_status = DependencyStatus::unsupported;
            return true;
        }
        if (image_.header.type != 2 && image_.header.type != 3) {
            image_.dependency_status = DependencyStatus::unsupported;
            warning("ELF_DEPENDENCIES_UNSUPPORTED", "Standard dependency extraction is supported only for ET_EXEC and ET_DYN.", dynamic->offset);
            return true;
        }
        if (dynamic->file_size % 16 != 0)
            return error("ELF_DYNAMIC_BOUNDS", "PT_DYNAMIC file size is not a multiple of the ELF64 dynamic-entry size.", dynamic->offset);
        const auto count = dynamic->file_size / 16;
        if (count > limits_.max_dynamic_entries)
            return error("ELF_LIMIT", "Dynamic-entry count exceeds the configured limit.", dynamic->offset);
        std::optional<std::uint64_t> string_address, string_size;
        std::vector<std::uint64_t> needed;
        bool terminated = false;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto offset = dynamic->offset + i * 16;
            std::uint64_t tag = 0, value = 0;
            if (!read(offset, tag) || !read(offset + 8, value))
                return error("ELF_DYNAMIC_BOUNDS", "Truncated dynamic entry.", offset);
            if (tag == 0) { terminated = true; break; }
            if (tag == 1) needed.push_back(value);
            if (tag == 5) {
                if (string_address) return error("ELF_DYNAMIC_AMBIGUOUS", "Duplicate DT_STRTAB entries are unsupported.", offset);
                string_address = value;
            }
            if (tag == 10) {
                if (string_size) return error("ELF_DYNAMIC_AMBIGUOUS", "Duplicate DT_STRSZ entries are unsupported.", offset);
                string_size = value;
            }
            if (tag >= 0x60000000ULL || tag > 34) {
                dependency_extensions_ = true;
                warning("ELF_DYNAMIC_TAG_UNSUPPORTED", "Unknown or extension-specific dynamic tags are not interpreted; dependency reporting may be incomplete.", offset);
            }
        }
        if (!terminated) return error("ELF_DYNAMIC_TERMINATOR", "PT_DYNAMIC has no DT_NULL terminator within its file-backed extent.", dynamic->offset);
        if ((!needed.empty() && (!string_address || !string_size)) || string_address.has_value() != string_size.has_value())
            return error("ELF_DYNAMIC_STRING", "Dynamic strings require both DT_STRTAB and DT_STRSZ.", dynamic->offset);
        image_.dependency_status = dependency_extensions_ ? DependencyStatus::unsupported : DependencyStatus::standard;
        if (!string_address) return true;
        if (*string_size > limits_.max_string_bytes)
            return error("ELF_LIMIT", "Dynamic string table exceeds the configured string-byte limit.", dynamic->offset);
        std::optional<std::uint64_t> string_offset;
        for (const auto& p : image_.programs) {
            if (p.type != pt_load || *string_address < p.virtual_address) continue;
            const auto delta = *string_address - p.virtual_address;
            if (delta > p.file_size || *string_size > p.file_size - delta) continue;
            std::uint64_t candidate = 0;
            if (!add(p.offset, delta, candidate) || !range(candidate, *string_size)) continue;
            if (string_offset && *string_offset != candidate)
                return error("ELF_DYNAMIC_AMBIGUOUS", "Dynamic string table maps to different file ranges through overlapping PT_LOAD segments.", dynamic->offset);
            string_offset = candidate;
        }
        if (!string_offset)
            return error("ELF_DYNAMIC_MAPPING", "DT_STRTAB and DT_STRSZ do not fit a single file-backed PT_LOAD range.", dynamic->offset);
        if (*string_size != 0 && (bytes_[static_cast<std::size_t>(*string_offset)] != 0 || bytes_[static_cast<std::size_t>(*string_offset + *string_size - 1)] != 0))
            return error("ELF_DYNAMIC_STRING", "A nonempty dynamic string table must begin and end with NUL bytes.", *string_offset);
        for (const auto index : needed) {
            std::string name;
            if (!string_at(*string_offset, *string_size, index, name, "ELF_DYNAMIC_STRING")) return false;
            image_.needed_libraries.push_back(std::move(name));
        }
        return true;
    }
};

} // namespace

ParseResult parse_elf(std::span<const std::uint8_t> bytes, const ParseLimits& limits) {
    return Parser(bytes, limits).run();
}

} // namespace ps5rt
