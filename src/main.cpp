#include "ps5rt/loader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>

namespace {
std::string hex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

// ELF strings are byte strings, not necessarily UTF-8. Escape every non-ASCII
// byte to preserve it reversibly and keep reports valid JSON and terminal-safe.
std::string quote(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32 || c >= 127) {
            out += "\\u00"; out += digits[c >> 4]; out += digits[c & 15];
        } else out += static_cast<char>(c);
    }
    return out + '"';
}

void diagnostics(std::ostream& out, const std::vector<ps5rt::Diagnostic>& items) {
    out << '[';
    bool first = true;
    for (const auto& d : items) {
        if (!first) out << ',';
        first = false;
        out << "{\"severity\":" << quote(d.severity == ps5rt::Severity::error ? "error" : "warning")
            << ",\"code\":" << quote(d.code) << ",\"message\":" << quote(d.message)
            << ",\"offset\":" << (d.offset ? quote(hex(*d.offset)) : "null") << '}';
    }
    out << ']';
}

void report(std::ostream& out, std::uint64_t size, const ps5rt::ParseResult& parsed,
            const ps5rt::PlanResult* planned) {
    out << "{\"schema_version\":1,\"file_size\":" << quote(hex(size))
        << ",\"inspection_status\":" << quote(parsed.ok() ? "accepted_subset" : "rejected")
        << ",\"execution_supported\":false,\"diagnostics\":";
    diagnostics(out, parsed.diagnostics);
    if (parsed.image) {
        const auto& elf = *parsed.image;
        const auto& h = elf.header;
        out << ",\"header\":{\"type\":" << h.type << ",\"machine\":" << h.machine
            << ",\"os_abi\":" << unsigned(h.os_abi) << ",\"abi_version\":" << unsigned(h.abi_version)
            << ",\"flags\":" << h.flags << ",\"entry\":" << quote(hex(h.entry))
            << ",\"program_count\":" << h.program_count << ",\"section_count\":" << h.section_count << '}'
            << ",\"programs\":[";
        bool first = true;
        for (const auto& p : elf.programs) {
            if (!first) out << ',';
            first = false;
            out << "{\"type\":" << p.type << ",\"flags\":" << p.flags
                << ",\"offset\":" << quote(hex(p.offset)) << ",\"virtual_address\":" << quote(hex(p.virtual_address))
                << ",\"file_size\":" << quote(hex(p.file_size)) << ",\"memory_size\":" << quote(hex(p.memory_size))
                << ",\"alignment\":" << quote(hex(p.alignment)) << '}';
        }
        out << "],\"sections\":[";
        first = true;
        for (const auto& s : elf.sections) {
            if (!first) out << ',';
            first = false;
            out << "{\"name\":" << quote(s.name) << ",\"type\":" << s.type
                << ",\"offset\":" << quote(hex(s.offset)) << ",\"size\":" << quote(hex(s.size))
                << ",\"address\":" << quote(hex(s.address)) << '}';
        }
        const char* status = elf.dependency_status == ps5rt::DependencyStatus::standard ? "standard" :
            elf.dependency_status == ps5rt::DependencyStatus::unsupported ? "unsupported" : "no_dynamic_table";
        out << "],\"dependencies\":{\"status\":" << quote(status) << ",\"needed\":[";
        first = true;
        for (const auto& name : elf.needed_libraries) {
            if (!first) out << ',';
            first = false;
            out << quote(name);
        }
        out << "]}";
    }
    if (planned) {
        out << ",\"plan_status\":" << quote(planned->ok() ? "offline_only" : "rejected")
            << ",\"plan_diagnostics\":";
        diagnostics(out, planned->diagnostics);
        if (planned->plan) {
            const auto& plan = *planned->plan;
            out << ",\"plan\":{\"base_address\":" << quote(hex(plan.base_address))
                << ",\"image_size\":" << quote(hex(plan.image_size)) << ",\"entry\":" << quote(hex(plan.entry))
                << ",\"regions\":[";
            bool first = true;
            for (const auto& r : plan.regions) {
                if (!first) out << ',';
                first = false;
                out << "{\"program_index\":" << r.program_index << ",\"virtual_address\":" << quote(hex(r.virtual_address))
                    << ",\"file_offset\":" << quote(hex(r.file_offset)) << ",\"file_size\":" << quote(hex(r.file_size))
                    << ",\"memory_size\":" << quote(hex(r.memory_size)) << ",\"flags\":" << r.flags << '}';
            }
            out << "]}";
        }
    }
    out << "}\n";
}

int io_error(bool json, const char* code, const char* message) {
    if (json) {
        std::cout << "{\"schema_version\":1,\"inspection_status\":\"not_inspected\",\"execution_supported\":false,\"diagnostics\":";
        diagnostics(std::cout, {{ps5rt::Severity::error, code, message, {}}});
        std::cout << "}\n";
    } else std::cerr << code << ": " << message << '\n';
    return 3;
}

int run(const std::vector<std::filesystem::path>& args) {
    const auto usage = [] {
        std::cout << "ps5rt 0.1.0 - experimental, non-executing ELF foundation\n"
                     "Usage: ps5rt inspect <file> [--json]\n"
                     "       ps5rt plan <file> [--json]\n"
                     "       ps5rt --help | --version\n"
                     "Acceptance is not proof of PS5 compatibility. No guest code runs.\n";
    };
    if (args.size() == 1 && args[0] == "--help") { usage(); return 0; }
    if (args.size() == 1 && args[0] == "--version") { std::cout << "ps5rt 0.1.0\n"; return 0; }
    if (args.size() < 2 || args.size() > 3 || (args[0] != "inspect" && args[0] != "plan") ||
        (args.size() == 3 && args[2] != "--json")) { usage(); return 2; }
    const bool json = args.size() == 3;
    try {
        const ps5rt::ParseLimits limits;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(args[1], ec) || ec)
            return io_error(json, "IO_FILE", "Input must be an accessible regular file.");
        const auto size = std::filesystem::file_size(args[1], ec);
        if (ec) return io_error(json, "IO_SIZE", "Could not determine input size.");
        if (size > limits.max_file_bytes || size > std::numeric_limits<std::size_t>::max() ||
            size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            return io_error(json, "IO_LIMIT", "Input exceeds the 256 MiB file limit.");
        std::ifstream input(args[1], std::ios::binary);
        if (!input) return io_error(json, "IO_OPEN", "Could not open input.");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            return io_error(json, "IO_READ", "Input read failed or the file changed size.");
        if (input.peek() != std::char_traits<char>::eof() || input.bad())
            return io_error(json, "IO_CHANGED", "Input changed size or could not be read completely.");
        const auto parsed = ps5rt::parse_elf(bytes, limits);
        std::optional<ps5rt::PlanResult> planned;
        if (parsed.ok() && args[0] == "plan") planned = ps5rt::make_load_plan(*parsed.image, size);
        if (json) report(std::cout, size, parsed, planned ? &*planned : nullptr);
        else {
            std::cout << "Inspection: " << (parsed.ok() ? "accepted by the implemented subset" : "rejected") << '\n';
            if (parsed.image) {
                const auto& e = *parsed.image;
                std::cout << "ELF64 little-endian; machine=" << e.header.machine << "; type=" << hex(e.header.type)
                    << "; entry=" << hex(e.header.entry) << '\n'
                    << e.programs.size() << " program headers; " << e.sections.size() << " section headers\n";
                for (const auto& name : e.needed_libraries) std::cout << "Needed: " << quote(name) << '\n';
                std::cout << "Dependency interpretation: " << (e.dependency_status == ps5rt::DependencyStatus::unsupported ?
                    "unsupported extensions; list may be incomplete" : e.dependency_status == ps5rt::DependencyStatus::standard ?
                    "standard DT_NEEDED only" : "no standard dynamic table found") << '\n';
            }
            for (const auto& d : parsed.diagnostics) std::cout << d.code << ": " << d.message << '\n';
            if (planned) {
                if (planned->plan) std::cout << "Offline layout: base=" << hex(planned->plan->base_address)
                    << ", span=" << hex(planned->plan->image_size) << ", regions=" << planned->plan->regions.size() << '\n';
                for (const auto& d : planned->diagnostics) std::cout << d.code << ": " << d.message << '\n';
            }
            std::cout << "Execution is not implemented. This is not a PS5 compatibility result.\n";
        }
        if (!parsed.ok()) return 1;
        if (planned && !planned->ok()) return 4;
        return 0;
    } catch (const std::bad_alloc&) {
        return io_error(json, "IO_RESOURCE", "Insufficient memory for inspection.");
    } catch (const std::exception&) {
        return io_error(json, "IO_EXCEPTION", "An unexpected host error prevented inspection.");
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    std::vector<std::filesystem::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
