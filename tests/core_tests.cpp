#include "fixture_builder.hpp"
#include "ps5rt/elf.hpp"
#include "ps5rt/loader.hpp"
#include "ps5rt/memory.hpp"
#include "ps5rt/startup.hpp"
#include <array>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace {

class Tests {
public:
    template <typename Function>
    void run(std::string_view name, Function function) {
        current_ = name;
        ++cases_;
        try {
            function();
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "FAIL " << current_ << ": exception: " << error.what() << '\n';
        } catch (...) {
            ++failures_;
            std::cerr << "FAIL " << current_ << ": unexpected exception\n";
        }
    }

    bool check(bool condition, std::string_view expression, int line) {
        ++checks_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL " << current_ << " at line " << line
                      << ": " << expression << '\n';
        }
        return condition;
    }

    int finish() const {
        std::cout << cases_ << " cases, " << checks_ << " checks, "
                  << failures_ << " failures\n";
        return failures_ == 0 ? 0 : 1;
    }

private:
    std::string_view current_;
    std::size_t cases_ = 0;
    std::size_t checks_ = 0;
    std::size_t failures_ = 0;
};

// Deliberately not assert(): these checks execute in Release builds too.
#define CHECK(expression) tests.check(static_cast<bool>(expression), #expression, __LINE__)
#define REQUIRE(expression) do { if (!CHECK(expression)) { return; } } while (false)

bool has_severity(const std::vector<ps5rt::Diagnostic>& diagnostics,
                  ps5rt::Severity severity) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [severity](const auto& item) { return item.severity == severity; });
}

std::string diagnostic_signature(const ps5rt::ParseResult& result) {
    std::string signature = result.ok() ? "ok" : "error";
    for (const auto& item : result.diagnostics) {
        signature += "|" + item.code + ":" + item.message;
        signature += item.severity == ps5rt::Severity::error ? ":E:" : ":W:";
        signature += item.offset ? std::to_string(*item.offset) : "none";
    }
    return signature;
}

void check_rejected(Tests& tests, const fixture::Bytes& bytes) {
    const auto result = ps5rt::parse_elf(bytes);
    CHECK(!result.ok());
    CHECK(has_severity(result.diagnostics, ps5rt::Severity::error));
}

void parser_tests(Tests& tests) {
    tests.run("minimal ELF without sections", [&] {
        const auto bytes = fixture::minimal();
        const auto result = ps5rt::parse_elf(bytes);
        REQUIRE(result.ok());
        CHECK(result.image->header.machine == 62);
        CHECK(result.image->header.entry == fixture::base_address);
        CHECK(result.image->programs.size() == 1);
        CHECK(result.image->sections.empty());
        CHECK(result.image->dependency_status == ps5rt::DependencyStatus::no_dynamic_table);
        CHECK(!has_severity(result.diagnostics, ps5rt::Severity::error));
    });

    tests.run("all header truncations reject", [&] {
        const auto bytes = fixture::minimal();
        for (std::size_t length = 0; length < fixture::elf_header_size; ++length) {
            const auto result = ps5rt::parse_elf(std::span(bytes).first(length));
            CHECK(!result.ok());
            CHECK(has_severity(result.diagnostics, ps5rt::Severity::error));
        }
    });

    tests.run("invalid identifying fields reject", [&] {
        for (const auto offset : {std::size_t{0}, std::size_t{4}, std::size_t{5},
                                  std::size_t{6}, std::size_t{20}}) {
            auto bytes = fixture::minimal();
            bytes[offset] = 0;
            check_rejected(tests, bytes);
        }
        auto bytes = fixture::minimal();
        bytes[4] = 1; // Supported milestone is ELF64, not ELF32.
        check_rejected(tests, bytes);
        bytes = fixture::minimal();
        bytes[5] = 2; // Big-endian input must not be misread as little-endian.
        check_rejected(tests, bytes);
    });

    tests.run("incorrect header and table entry sizes reject", [&] {
        auto bytes = fixture::minimal();
        fixture::u16(bytes, 52, 63);
        check_rejected(tests, bytes);
        bytes = fixture::minimal();
        fixture::u16(bytes, 54, 55);
        check_rejected(tests, bytes);
        bytes = fixture::with_sections();
        fixture::u16(bytes, 58, 63);
        check_rejected(tests, bytes);
    });

    tests.run("table bounds and arithmetic overflow reject", [&] {
        for (const auto offset : {std::uint64_t{260},
                                  std::numeric_limits<std::uint64_t>::max() - 4}) {
            auto bytes = fixture::minimal();
            fixture::u64(bytes, 32, offset);
            check_rejected(tests, bytes);
        }
        auto bytes = fixture::with_sections();
        fixture::u64(bytes, 40, bytes.size() - 32);
        check_rejected(tests, bytes);
        fixture::u64(bytes, 40, std::numeric_limits<std::uint64_t>::max() - 4);
        check_rejected(tests, bytes);
    });

    tests.run("extended numbering is explicitly unsupported", [&] {
        auto bytes = fixture::minimal();
        fixture::u16(bytes, 56, 0xffff); // PN_XNUM
        check_rejected(tests, bytes);
        bytes = fixture::with_sections();
        fixture::u16(bytes, 60, 0); // Real count would live in section zero.
        check_rejected(tests, bytes);
        bytes = fixture::with_sections();
        fixture::u16(bytes, 62, 0xffff); // SHN_XINDEX
        check_rejected(tests, bytes);
    });

    tests.run("file and table resource caps reject", [&] {
        const auto bytes = fixture::minimal();
        ps5rt::ParseLimits limits;
        limits.max_file_bytes = bytes.size() - 1;
        CHECK(!ps5rt::parse_elf(bytes, limits).ok());
        limits = {};
        limits.max_program_headers = 0;
        CHECK(!ps5rt::parse_elf(bytes, limits).ok());
        limits = {};
        limits.max_section_headers = 3;
        CHECK(!ps5rt::parse_elf(fixture::with_sections(), limits).ok());
    });

    tests.run("LOAD bounds size and address wrapping reject", [&] {
        auto bytes = fixture::minimal();
        fixture::u64(bytes, fixture::ph(0) + 32, 33);
        check_rejected(tests, bytes);
        bytes = fixture::minimal();
        fixture::u64(bytes, fixture::ph(0) + 8, bytes.size() - 8);
        fixture::u64(bytes, fixture::ph(0) + 48, 1);
        check_rejected(tests, bytes);
        bytes = fixture::minimal();
        fixture::u64(bytes, fixture::ph(0) + 8,
                     std::numeric_limits<std::uint64_t>::max() - 8);
        check_rejected(tests, bytes);
        bytes = fixture::minimal();
        fixture::u64(bytes, fixture::ph(0) + 16,
                     std::numeric_limits<std::uint64_t>::max() - 8);
        fixture::u64(bytes, fixture::ph(0) + 48, 1);
        check_rejected(tests, bytes);
    });

    tests.run("LOAD alignment and congruence", [&] {
        auto bytes = fixture::minimal();
        fixture::u64(bytes, fixture::ph(0) + 48, 3);
        check_rejected(tests, bytes);
        bytes = fixture::minimal();
        fixture::u64(bytes, fixture::ph(0) + 16, fixture::base_address + 1);
        check_rejected(tests, bytes);
        for (const auto alignment : {std::uint64_t{0}, std::uint64_t{1}}) {
            bytes = fixture::minimal();
            fixture::u64(bytes, fixture::ph(0) + 48, alignment);
            CHECK(ps5rt::parse_elf(bytes).ok());
        }
    });

    tests.run("section names and NOBITS", [&] {
        const auto result = ps5rt::parse_elf(fixture::with_sections());
        REQUIRE(result.ok());
        REQUIRE(result.image->sections.size() == 4);
        CHECK(result.image->sections[1].name == ".text");
        CHECK(result.image->sections[2].name == ".bss");
        CHECK(result.image->sections[3].name == ".shstrtab");
        CHECK(result.image->sections[2].type == 8);
        CHECK(result.image->sections[2].offset == std::numeric_limits<std::uint64_t>::max());
    });

    tests.run("no section-name table means no fabricated names", [&] {
        auto bytes = fixture::with_sections();
        fixture::u16(bytes, 62, 0);
        const auto result = ps5rt::parse_elf(bytes);
        REQUIRE(result.ok());
        for (const auto& section : result.image->sections) {
            CHECK(section.name.empty());
        }
    });

    tests.run("invalid section payload names and alignment reject", [&] {
        auto bytes = fixture::with_sections();
        fixture::u64(bytes, fixture::sh(1) + 24, bytes.size() - 1);
        check_rejected(tests, bytes);
        bytes = fixture::with_sections();
        fixture::u32(bytes, fixture::sh(1), 10000);
        check_rejected(tests, bytes);
        bytes = fixture::with_sections();
        bytes.back() = 'X'; // Last name now lacks a NUL within the name table.
        check_rejected(tests, bytes);
        bytes = fixture::with_sections();
        fixture::u16(bytes, 62, 4);
        check_rejected(tests, bytes);
        bytes = fixture::with_sections();
        fixture::u64(bytes, fixture::sh(1) + 48, 3);
        check_rejected(tests, bytes);
    });

    tests.run("non-x86 and vendor files remain inspectable", [&] {
        auto bytes = fixture::minimal();
        fixture::u16(bytes, 18, 183); // EM_AARCH64: inspection, not execution.
        auto result = ps5rt::parse_elf(bytes);
        REQUIRE(result.ok());
        CHECK(result.image->header.machine == 183);
        CHECK(!ps5rt::make_load_plan(*result.image, bytes.size()).ok());
        bytes = fixture::minimal();
        fixture::u16(bytes, 16, 0xfe01); // Invented OS-specific type, not a Sony constant.
        result = ps5rt::parse_elf(bytes);
        REQUIRE(result.ok());
        CHECK(has_severity(result.diagnostics, ps5rt::Severity::warning));
        CHECK(!ps5rt::make_load_plan(*result.image, bytes.size()).ok());
    });
}

void dynamic_tests(Tests& tests) {
    tests.run("standard dynamic dependency extraction", [&] {
        const auto result = ps5rt::parse_elf(fixture::with_dynamic());
        REQUIRE(result.ok());
        CHECK(result.image->dependency_status == ps5rt::DependencyStatus::standard);
        REQUIRE(result.image->needed_libraries.size() == 1);
        CHECK(result.image->needed_libraries.front() == fixture::library_name);
    });

    tests.run("dynamic string offset and missing NUL reject", [&] {
        auto bytes = fixture::with_dynamic();
        fixture::dynamic_entry(bytes, 2, 1, sizeof(fixture::library_name) + 1);
        check_rejected(tests, bytes);
        bytes = fixture::with_dynamic();
        bytes[fixture::dynamic_strings_offset + sizeof(fixture::library_name)] = 'X';
        check_rejected(tests, bytes);
    });

    tests.run("unmapped and oversized dynamic strings reject", [&] {
        auto bytes = fixture::with_dynamic();
        fixture::dynamic_entry(bytes, 0, 5, 0x9000);
        check_rejected(tests, bytes);
        bytes = fixture::with_dynamic();
        fixture::dynamic_entry(bytes, 1, 10, 0x10000);
        check_rejected(tests, bytes);
        bytes = fixture::with_dynamic();
        fixture::dynamic_entry(bytes, 0, 5, std::numeric_limits<std::uint64_t>::max() - 2);
        check_rejected(tests, bytes);
    });

    tests.run("dynamic terminator and unique table metadata required", [&] {
        auto bytes = fixture::with_dynamic();
        fixture::dynamic_entry(bytes, 3, 1, 1); // No DT_NULL within PT_DYNAMIC.
        check_rejected(tests, bytes);
        bytes = fixture::with_dynamic();
        fixture::dynamic_entry(bytes, 2, 5, fixture::dynamic_strings_address);
        check_rejected(tests, bytes);
        bytes = fixture::with_dynamic();
        fixture::u64(bytes, fixture::ph(1) + 32, 63);
        check_rejected(tests, bytes);
    });

    tests.run("dynamic entry and string resource limits", [&] {
        ps5rt::ParseLimits limits;
        limits.max_dynamic_entries = 3;
        CHECK(!ps5rt::parse_elf(fixture::with_dynamic(), limits).ok());
        limits = {};
        limits.max_string_bytes = 4;
        CHECK(!ps5rt::parse_elf(fixture::with_dynamic(), limits).ok());
    });
}

void loader_tests(Tests& tests) {
    tests.run("offline materialization copies and zero-fills", [&] {
        const auto bytes = fixture::minimal();
        const auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        const auto plan = ps5rt::make_load_plan(*parsed.image, bytes.size());
        REQUIRE(plan.ok());
        CHECK(plan.plan->base_address == fixture::base_address);
        CHECK(plan.plan->image_size == 32);
        CHECK(plan.plan->entry == fixture::base_address);
        REQUIRE(plan.plan->regions.size() == 1);
        CHECK(plan.plan->regions[0].file_size == 16);
        const auto result = ps5rt::materialize_image(bytes, *parsed.image);
        REQUIRE(result.ok());
        REQUIRE(result.image->bytes.size() == 32);
        for (std::size_t index = 0; index < 16; ++index) {
            CHECK(result.image->bytes[index] == bytes[fixture::data_offset + index]);
        }
        CHECK(std::all_of(result.image->bytes.begin() + 16, result.image->bytes.end(),
                          [](auto byte) { return byte == 0; }));
    });

    tests.run("multiple regions have zero-filled gaps and tails", [&] {
        const auto bytes = fixture::two_loads();
        const auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        const auto result = ps5rt::materialize_image(bytes, *parsed.image);
        REQUIRE(result.ok());
        REQUIRE(result.image->bytes.size() == 0x210);
        CHECK(result.image->plan.regions.size() == 2);
        CHECK(std::all_of(result.image->bytes.begin() + 16, result.image->bytes.begin() + 0x200,
                          [](auto byte) { return byte == 0; }));
        for (std::size_t index = 0; index < 8; ++index) {
            CHECK(result.image->bytes[0x200 + index] == bytes[0x200 + index]);
            CHECK(result.image->bytes[0x208 + index] == 0);
        }
    });

    tests.run("overlapping nonempty LOAD memory ranges reject", [&] {
        const auto bytes = fixture::two_loads();
        auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        parsed.image->programs[1].virtual_address = fixture::base_address + 16;
        parsed.image->programs[1].alignment = 1;
        CHECK(!ps5rt::make_load_plan(*parsed.image, bytes.size()).ok());
        CHECK(!ps5rt::materialize_image(bytes, *parsed.image).ok());
    });

    tests.run("touching but nonoverlapping LOAD ranges allowed", [&] {
        const auto bytes = fixture::two_loads();
        auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        parsed.image->programs[1].virtual_address = fixture::base_address + 32;
        parsed.image->programs[1].alignment = 1;
        const auto result = ps5rt::materialize_image(bytes, *parsed.image);
        REQUIRE(result.ok());
        CHECK(result.image->bytes.size() == 48);
    });

    tests.run("entry must lie in executable file-backed LOAD bytes", [&] {
        const auto bytes = fixture::minimal();
        const auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        for (const auto entry : {fixture::base_address - 1, fixture::base_address + 16,
                                  fixture::base_address + 32}) {
            auto changed = *parsed.image;
            changed.header.entry = entry;
            CHECK(!ps5rt::make_load_plan(changed, bytes.size()).ok());
        }
        auto changed = *parsed.image;
        changed.programs[0].flags = 6; // Read/write, not executable.
        CHECK(!ps5rt::make_load_plan(changed, bytes.size()).ok());
    });

    tests.run("zero entry is allowed but warned", [&] {
        const auto bytes = fixture::minimal();
        auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        parsed.image->header.entry = 0;
        const auto result = ps5rt::make_load_plan(*parsed.image, bytes.size());
        REQUIRE(result.ok());
        CHECK(has_severity(result.diagnostics, ps5rt::Severity::warning));
    });

    tests.run("ET_DYN layout retains addresses and has explicit limits", [&] {
        auto bytes = fixture::minimal();
        fixture::u16(bytes, 16, 3);
        const auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        const auto result = ps5rt::materialize_image(bytes, *parsed.image);
        REQUIRE(result.ok());
        CHECK(result.image->plan.base_address == fixture::base_address);
        CHECK(result.image->plan.entry == fixture::base_address);
        CHECK(has_severity(result.diagnostics, ps5rt::Severity::warning));
    });

    tests.run("no loadable memory and unsupported ELF type reject", [&] {
        const auto bytes = fixture::minimal();
        const auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        auto changed = *parsed.image;
        changed.programs.clear();
        CHECK(!ps5rt::make_load_plan(changed, bytes.size()).ok());
        changed = *parsed.image;
        changed.programs[0].file_size = 0;
        changed.programs[0].memory_size = 0;
        CHECK(!ps5rt::make_load_plan(changed, bytes.size()).ok());
        changed = *parsed.image;
        changed.header.type = 1; // ET_REL is inspectable, not a runnable layout.
        CHECK(!ps5rt::make_load_plan(changed, bytes.size()).ok());
    });

    tests.run("loader revalidates bounds instead of trusting its caller", [&] {
        const auto bytes = fixture::minimal();
        const auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        CHECK(!ps5rt::make_load_plan(*parsed.image, bytes.size() - 1).ok());
        CHECK(!ps5rt::materialize_image(std::span(bytes).first(bytes.size() - 1), *parsed.image).ok());
        auto changed = *parsed.image;
        changed.programs[0].file_size = changed.programs[0].memory_size + 1;
        CHECK(!ps5rt::make_load_plan(changed, bytes.size()).ok());
        changed = *parsed.image;
        changed.programs[0].virtual_address = std::numeric_limits<std::uint64_t>::max() - 8;
        CHECK(!ps5rt::make_load_plan(changed, bytes.size()).ok());
    });

    tests.run("image limits count sparse gaps and apply before allocation", [&] {
        const auto bytes = fixture::two_loads();
        auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        ps5rt::LoaderLimits limits;
        limits.max_image_bytes = 0x20f;
        CHECK(!ps5rt::make_load_plan(*parsed.image, bytes.size(), limits).ok());
        CHECK(!ps5rt::materialize_image(bytes, *parsed.image, limits).ok());
        limits.max_image_bytes = 0x210;
        CHECK(ps5rt::make_load_plan(*parsed.image, bytes.size(), limits).ok());
        parsed.image->programs[1].virtual_address = fixture::base_address + 64ULL * 1024 * 1024;
        CHECK(!ps5rt::make_load_plan(*parsed.image, bytes.size()).ok());
    });
}

void memory_tests(Tests& tests) {
    tests.run("guest memory permissions gaps and zero initialization", [&] {
        const auto file = fixture::two_loads();
        const auto parsed = ps5rt::parse_elf(file);
        REQUIRE(parsed.ok());
        auto built = ps5rt::make_guest_memory(file, *parsed.image);
        REQUIRE(built.ok());
        auto& memory = *built.memory;
        const auto check_code = [&](const std::optional<ps5rt::Diagnostic>& error, std::string_view code) {
            CHECK(error.has_value());
            if (error) CHECK(error->code == code);
        };
        std::array<std::uint8_t, 8> bytes{};
        CHECK(!memory.read(0x1000, bytes));
        CHECK(bytes[0] == 0xd0 && bytes[7] == 0xd7);
        CHECK(!memory.check_execute(0x1000, 16));
        const auto denied_write = memory.write(0x1000, bytes);
        REQUIRE(denied_write.has_value());
        CHECK(denied_write->code == "MEMORY_PERMISSION");
        CHECK(memory.check_execute(0x1200, 1).has_value());
        bytes.fill(0xff);
        CHECK(!memory.read(0x1208, bytes));
        CHECK(std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0; }));
        bytes.fill(0x5a);
        CHECK(!memory.write(0x1208, bytes));
        bytes.fill(0);
        CHECK(!memory.read(0x1208, bytes));
        CHECK(bytes[0] == 0x5a && bytes[7] == 0x5a);
        std::array<std::uint8_t, 16> too_long{};
        too_long.fill(0xcc);
        CHECK(memory.write(0x1208, too_long).has_value());
        CHECK(!memory.read(0x1208, bytes));
        CHECK(std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0x5a; }));
        check_code(memory.read(0x1100, bytes), "MEMORY_UNMAPPED");
        check_code(memory.read(0x0fff, bytes), "MEMORY_UNMAPPED");
        check_code(memory.read(0x1210, bytes), "MEMORY_UNMAPPED");
        check_code(memory.read(std::numeric_limits<std::uint64_t>::max() - 3, bytes), "MEMORY_OVERFLOW");
        CHECK(!memory.read(std::numeric_limits<std::uint64_t>::max(), {}));
        CHECK(!memory.write(0, {}));
        CHECK(!memory.check_execute(0, 0));
    });
    tests.run("cross-segment access checks all permissions before copying", [&] {
        const auto file = fixture::two_loads();
        auto parsed = ps5rt::parse_elf(file);
        REQUIRE(parsed.ok());
        parsed.image->header.entry = 0;
        auto& first = parsed.image->programs[0];
        auto& second = parsed.image->programs[1];
        first.flags = 6;
        second.virtual_address = first.virtual_address + first.memory_size;
        second.alignment = 1;
        second.flags = 4;
        auto built = ps5rt::make_guest_memory(file, *parsed.image);
        REQUIRE(built.ok());
        std::array<std::uint8_t, 16> before{};
        CHECK(!built.memory->read(0x1018, before));
        auto replacement = before;
        replacement.fill(0xcc);
        REQUIRE(built.memory->write(0x1018, replacement).has_value());
        std::array<std::uint8_t, 16> after{};
        CHECK(!built.memory->read(0x1018, after));
        CHECK(before == after);
        second.flags = 6;
        built = ps5rt::make_guest_memory(file, *parsed.image);
        REQUIRE(built.ok());
        CHECK(!built.memory->write(0x1018, replacement));
        CHECK(!built.memory->read(0x1018, after));
        CHECK(after == replacement);
        after.fill(0x77);
        CHECK(built.memory->read(0x1028, after).has_value());
        CHECK(std::all_of(after.begin(), after.end(), [](auto b) { return b == 0x77; }));
    });
    tests.run("guest memory creation preserves loader rejection and limits", [&] {
        const auto file = fixture::minimal();
        auto parsed = ps5rt::parse_elf(file);
        REQUIRE(parsed.ok());
        CHECK(!ps5rt::make_guest_memory(file, *parsed.image, {31}).ok());
        parsed.image->header.machine = 183;
        CHECK(!ps5rt::make_guest_memory(file, *parsed.image).ok());
    });
    tests.run("guest reads match an independent bounded-range oracle", [&] {
        const auto file = fixture::two_loads();
        const auto parsed = ps5rt::parse_elf(file);
        REQUIRE(parsed.ok());
        auto built = ps5rt::make_guest_memory(file, *parsed.image);
        REQUIRE(built.ok());
        for (std::uint64_t address = 0xff8; address <= 0x1218; ++address) {
            for (const std::size_t size : {std::size_t{1}, std::size_t{8}, std::size_t{32}}) {
                std::array<std::uint8_t, 32> buffer{};
                const bool expected = (address >= 0x1000 && address + size <= 0x1020) ||
                                      (address >= 0x1200 && address + size <= 0x1210);
                const auto error = built.memory->read(address, std::span(buffer).first(size));
                CHECK(error.has_value() != expected);
            }
        }
    });
}

void startup_tests(Tests& tests) {
    tests.run("synthetic startup pointers, strings, alignment and zero fill", [&] {
        const std::vector<std::vector<std::string>> cases{
            {}, {""}, {"probe", "alpha"}, {std::string("\x80\xff", 2), ""},
            {std::string(257, 'x'), "tail"}, std::vector<std::string>(64, "a")};
        for (const auto& arguments : cases) {
            constexpr std::uint64_t base = 0x12345000;
            const auto built = ps5rt::build_startup_stack(base, 4096, arguments);
            REQUIRE(built.ok());
            CHECK(built.diagnostics.empty());
            const auto& stack = *built.stack;
            const auto frame = static_cast<std::size_t>(stack.frame_address - base);
            CHECK(stack.base_address == base && stack.bytes.size() == 4096);
            REQUIRE(frame >= 512 && frame + (arguments.size() + 2) * 8 <= stack.bytes.size());
            CHECK(stack.frame_address % 16 == 0);
            CHECK(stack.argc == arguments.size() && stack.argv_address == stack.frame_address + 8);
            const auto read64 = [&](std::size_t offset) {
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < 8; ++i)
                    value |= static_cast<std::uint64_t>(stack.bytes.at(offset + i)) << (8 * i);
                return value;
            };
            CHECK(read64(frame) == arguments.size());
            CHECK(read64(frame + 8 + arguments.size() * 8) == 0);
            const auto headroom = std::span(stack.bytes).first(frame);
            CHECK(std::all_of(headroom.begin(), headroom.end(), [](auto b) { return b == 0; }));
            std::size_t bytes_needed = 0;
            for (const auto& argument : arguments) bytes_needed += argument.size() + 1;
            auto cursor = stack.bytes.size() - bytes_needed;
            const auto frame_end = frame + (arguments.size() + 2) * 8;
            const auto padding = std::span(stack.bytes).subspan(frame_end, cursor - frame_end);
            CHECK(std::all_of(padding.begin(), padding.end(), [](auto b) { return b == 0; }));
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                CHECK(read64(frame + 8 + i * 8) == base + cursor);
                for (const char byte : arguments[i]) CHECK(stack.bytes.at(cursor++) == static_cast<unsigned char>(byte));
                CHECK(stack.bytes.at(cursor++) == 0);
            }
            CHECK(cursor == stack.bytes.size());
            const auto repeated = ps5rt::build_startup_stack(base, 4096, arguments);
            REQUIRE(repeated.ok());
            CHECK(repeated.stack->bytes == stack.bytes);
        }
    });
    tests.run("synthetic startup rejects invalid dimensions and arguments", [&] {
        const auto rejected = [&](std::uint64_t base, std::uint64_t size,
                                  const std::vector<std::string>& args, std::string_view code) {
            const auto result = ps5rt::build_startup_stack(base, size, args);
            CHECK(!result.ok() && !result.stack);
            REQUIRE(result.diagnostics.size() == 1);
            CHECK(result.diagnostics.front().code == code);
            CHECK(result.diagnostics.front().severity == ps5rt::Severity::error);
        };
        rejected(1, 4096, {}, "STARTUP_ALIGNMENT");
        rejected(0, 4097, {}, "STARTUP_ALIGNMENT");
        rejected(0, 0, {}, "STARTUP_SIZE");
        rejected(0, 4080, {}, "STARTUP_SIZE");
        rejected(0, 1024 * 1024 + 16, {}, "STARTUP_SIZE");
        rejected(std::numeric_limits<std::uint64_t>::max() - 15, 4096, {}, "STARTUP_OVERFLOW");
        rejected(0, 4096, std::vector<std::string>(65), "STARTUP_ARGUMENT_COUNT");
        rejected(0, 4096, {std::string("a\0b", 3)}, "STARTUP_ARGUMENT_NUL");
        rejected(0, 65536, {std::string(16384, 'a')}, "STARTUP_ARGUMENT_BYTES");
        rejected(0, 65536, {std::string(16383, 'a'), ""}, "STARTUP_ARGUMENT_BYTES");
        rejected(0, 4096, {std::string(4095, 'a')}, "STARTUP_SPACE");
        rejected(0, 4096, {std::string(3580, 'a')}, "STARTUP_HEADROOM");
    });
    tests.run("synthetic startup accepts exact policy boundaries", [&] {
        auto result = ps5rt::build_startup_stack(0, 65536, std::vector<std::string>{std::string(16383, 'a')});
        REQUIRE(result.ok());
        CHECK(result.stack->bytes.back() == 0);
        result = ps5rt::build_startup_stack(0, 1024 * 1024, {});
        REQUIRE(result.ok());
        CHECK(result.stack->bytes.size() == 1024 * 1024);
        // 3559 bytes + terminator + 24-byte frame leaves exactly 512 bytes.
        result = ps5rt::build_startup_stack(0, 4096, std::vector<std::string>{std::string(3559, 'a')});
        REQUIRE(result.ok());
        CHECK(result.stack->frame_address == 512);
        result = ps5rt::build_startup_stack(0, 4096, std::vector<std::string>{std::string(3560, 'a')});
        CHECK(!result.ok());
    });
}

void bounded_mutation_tests(Tests& tests) {
    tests.run("NULL program fields are undefined and ignored", [&] {
        auto bytes = fixture::two_loads();
        fixture::u32(bytes, fixture::ph(1), 0);
        fixture::u64(bytes, fixture::ph(1) + 8, std::numeric_limits<std::uint64_t>::max());
        fixture::u64(bytes, fixture::ph(1) + 32, std::numeric_limits<std::uint64_t>::max());
        fixture::u64(bytes, fixture::ph(1) + 48, 3);
        const auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        const auto loaded = ps5rt::materialize_image(bytes, *parsed.image);
        REQUIRE(loaded.ok());
        CHECK(loaded.image->plan.regions.size() == 1);
    });

    tests.run("memory-only LOAD copies no source bytes", [&] {
        const auto bytes = fixture::two_loads();
        auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        parsed.image->programs[1].file_size = 0;
        // Direct API callers need no valid source pointer for an empty extent.
        parsed.image->programs[1].offset = std::numeric_limits<std::uint64_t>::max();
        parsed.image->programs[1].alignment = 1;
        const auto loaded = ps5rt::materialize_image(bytes, *parsed.image);
        REQUIRE(loaded.ok());
        CHECK(std::all_of(loaded.image->bytes.begin() + 0x200, loaded.image->bytes.end(),
                          [](auto byte) { return byte == 0; }));
    });

    tests.run("loader rejects unsupported flags and alignment independently", [&] {
        const auto bytes = fixture::minimal();
        auto parsed = ps5rt::parse_elf(bytes);
        REQUIRE(parsed.ok());
        parsed.image->programs[0].flags |= 0x10000000;
        CHECK(!ps5rt::make_load_plan(*parsed.image, bytes.size()).ok());
        parsed.image->programs[0].flags = 5;
        parsed.image->programs[0].alignment = 3;
        CHECK(!ps5rt::make_load_plan(*parsed.image, bytes.size()).ok());
    });

    tests.run("all prefixes of a valid image are bounded and deterministic", [&] {
        const auto bytes = fixture::minimal();
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            const auto prefix = std::span(bytes).first(length);
            const auto first = ps5rt::parse_elf(prefix);
            const auto second = ps5rt::parse_elf(prefix);
            CHECK(!first.ok()); // The last file-backed byte is required.
            CHECK(diagnostic_signature(first) == diagnostic_signature(second));
        }
    });

    tests.run("seeded bounded mutations preserve result invariants", [&] {
        std::mt19937 generator(0x505335U);
        ps5rt::ParseLimits limits;
        limits.max_file_bytes = 4096;
        limits.max_program_headers = 8;
        limits.max_section_headers = 8;
        limits.max_dynamic_entries = 16;
        limits.max_string_bytes = 256;
        ps5rt::LoaderLimits loader_limits;
        loader_limits.max_image_bytes = 4096;
        for (std::size_t iteration = 0; iteration < 128; ++iteration) {
            auto bytes = (iteration % 2 == 0) ? fixture::minimal() : fixture::with_dynamic();
            const auto mutations = 1U + generator() % 4U;
            for (std::uint32_t mutation = 0; mutation < mutations; ++mutation) {
                const auto offset = static_cast<std::size_t>(generator()) % bytes.size();
                bytes[offset] ^= static_cast<std::uint8_t>(1U + generator() % 255U);
            }
            const auto first = ps5rt::parse_elf(bytes, limits);
            const auto second = ps5rt::parse_elf(bytes, limits);
            CHECK(diagnostic_signature(first) == diagnostic_signature(second));
            if (first.ok()) {
                CHECK(!has_severity(first.diagnostics, ps5rt::Severity::error));
                CHECK(first.image->programs.size() <= limits.max_program_headers);
                CHECK(first.image->sections.size() <= limits.max_section_headers);
                const auto loaded = ps5rt::materialize_image(bytes, *first.image, loader_limits);
                if (loaded.ok()) {
                    CHECK(loaded.image->bytes.size() <= loader_limits.max_image_bytes);
                    CHECK(loaded.image->bytes.size() == loaded.image->plan.image_size);
                } else {
                    CHECK(has_severity(loaded.diagnostics, ps5rt::Severity::error));
                }
            } else {
                CHECK(has_severity(first.diagnostics, ps5rt::Severity::error));
            }
        }
    });
}

} // namespace

int main() {
    Tests tests;
    parser_tests(tests);
    dynamic_tests(tests);
    loader_tests(tests);
    memory_tests(tests);
    startup_tests(tests);
    bounded_mutation_tests(tests);
    return tests.finish();
}
