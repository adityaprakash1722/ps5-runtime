// Fixed, original x86-64 instruction fixtures. This is not an ELF/PS5 runner.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <utility>
#include "ps5rt/memory.hpp"
#include "ps5rt/startup.hpp"
#include "loaded_fixture.hpp"
#include "stack_fixture.hpp"

#if defined(_WIN32) && defined(_M_X64)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#else
#error Native probe requires Windows MSVC x64 or Linux x86-64.
#endif

namespace {
class Pages {
public:
    explicit Pages(std::size_t middle_pages = 1) {
#ifdef _WIN32
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page_ = info.dwPageSize;
#else
        const auto size = sysconf(_SC_PAGESIZE);
        if (size <= 0) throw std::runtime_error("PROBE_PAGE_SIZE");
        page_ = static_cast<std::size_t>(size);
#endif
        if (page_ < 64 || page_ > 64 * 1024 * 1024 || middle_pages == 0 ||
            middle_pages > (64 * 1024 * 1024) / page_)
            throw std::runtime_error("PROBE_PAGE_SIZE");
        span_ = page_ * middle_pages;
        total_ = span_ + 2 * page_;
#ifdef _WIN32
        base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, total_,
            MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        if (!base_) throw std::runtime_error("PROBE_ALLOCATE");
        DWORD old = 0;
        if (!VirtualProtect(data(), span_, PAGE_READWRITE, &old)) {
            release();
            throw std::runtime_error("PROBE_WRITABLE");
        }
#else
        auto* mapped = mmap(nullptr, total_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) throw std::runtime_error("PROBE_ALLOCATE");
        base_ = static_cast<std::uint8_t*>(mapped);
        if (mprotect(data(), span_, PROT_READ | PROT_WRITE) != 0) {
            release();
            throw std::runtime_error("PROBE_WRITABLE");
        }
#endif
    }
    Pages(const Pages&) = delete;
    Pages& operator=(const Pages&) = delete;
    ~Pages() { release(); }
    std::uint8_t* data() const { return base_ + page_; }
    std::uint8_t* guard() const { return base_; }
    std::size_t size() const { return span_; }
    std::size_t page_size() const { return page_; }
    void protect_page(std::size_t index, std::uint32_t flags) {
        if (index >= span_ / page_ || (flags != 0 && flags != 4 && flags != 5 && flags != 6))
            throw std::runtime_error("PROBE_PAGE_POLICY");
        auto* page = data() + index * page_;
#ifdef _WIN32
        const DWORD permission = flags == 0 ? PAGE_NOACCESS : flags == 4 ? PAGE_READONLY :
            flags == 5 ? PAGE_EXECUTE_READ : PAGE_READWRITE;
        DWORD old = 0;
        if (!VirtualProtect(page, page_, permission, &old)) throw std::runtime_error("PROBE_PROTECT");
        if ((flags & 1U) && !FlushInstructionCache(GetCurrentProcess(), page, page_))
            throw std::runtime_error("PROBE_CACHE");
#else
        const int permission = flags == 0 ? PROT_NONE : flags == 4 ? PROT_READ :
            flags == 5 ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE;
        if (mprotect(page, page_, permission) != 0) throw std::runtime_error("PROBE_PROTECT");
        if (flags & 1U) __builtin___clear_cache(reinterpret_cast<char*>(page), reinterpret_cast<char*>(page + page_));
#endif
    }
    void seal() {
        for (std::size_t i = 0; i < span_ / page_; ++i) protect_page(i, 5);
    }
private:
    void release() noexcept {
        if (!base_) return;
#ifdef _WIN32
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        munmap(base_, total_);
#endif
        base_ = nullptr;
    }
    std::uint8_t* base_ = nullptr;
    std::size_t page_ = 0;
    std::size_t span_ = 0;
    std::size_t total_ = 0;
};

// Clang's function-type sanitizer reads metadata preceding a compiled function.
// Raw generated code has no such metadata. Suppress ONLY that check at this
// implementation-defined native-code boundary; other host checks remain enabled.
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
std::uint64_t invoke(std::uint8_t* code, std::uint8_t* state) {
    using Function = std::uint64_t (*)(void*);
    return reinterpret_cast<Function>(code)(state);
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
std::uint64_t invoke_leaf(std::uint8_t* code) {
    using Function = std::uint64_t (*)();
    return reinterpret_cast<Function>(code)();
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
std::uint64_t invoke_stack(std::uint8_t* bridge, std::uint8_t* entry, std::uint8_t* frame, void* record) {
    using Function = std::uint64_t (*)(void*, void*, void*);
    return reinterpret_cast<Function>(bridge)(entry, frame, record);
}

std::vector<std::uint8_t> program(std::string_view mode) {
    // ENDBR64 supports hosts enforcing indirect-branch tracking.
    std::vector<std::uint8_t> bytes{0xf3, 0x0f, 0x1e, 0xfa};
#ifndef _WIN32
    // System V passes the first pointer in RDI; our fixture uses RCX.
    bytes.insert(bytes.end(), {0x48, 0x89, 0xf9}); // mov rcx, rdi
#endif
    if (mode == "arithmetic") {
        bytes.insert(bytes.end(), {
            0x48, 0x8b, 0x01,             // mov rax, [rcx]       ; a
            0x48, 0x03, 0x41, 0x08,       // add rax, [rcx+8]     ; + b, modulo 2^64
            0x48, 0x6b, 0xc0, 0x03,       // imul rax, rax, 3     ; low 64 bits
            0x48, 0x89, 0x41, 0x10,       // mov [rcx+16], rax    ; store product
            0x48, 0x33, 0x41, 0x18,       // xor rax, [rcx+24]    ; mask
            0xc3                         // ret                 ; RAX is return value
        });
    } else if (mode == "illegal-instruction") {
        bytes.insert(bytes.end(), {0x0f, 0x0b}); // ud2: deliberately invalid instruction
    } else if (mode == "write-code") {
        bytes.insert(bytes.end(), {0xc6, 0x05, 0xf9, 0xff, 0xff, 0xff, 0x00, 0xc3});
        // mov byte [rip-7], 0: write the start of this instruction on an RX page.
    } else if (mode == "guard-read") {
        bytes.insert(bytes.end(), {0x48, 0x8b, 0x01, 0xc3}); // mov rax, [rcx]; ret
    } else if (mode == "hang") {
        bytes.insert(bytes.end(), {0xeb, 0xfe}); // jmp to self: supervisor must kill/reap
    } else { // non-executable: page stays RW, so fetching this RET must fault.
        bytes.push_back(0xc3);
    }
    return bytes;
}

void hex_value(std::uint64_t value) { std::cout << '"' << "0x" << std::hex << value << std::dec << '"'; }

ps5rt::GuestMemory prepare_loaded_mapping(Pages& mapping, std::span<const std::uint8_t> payload = {}) {
    const auto file = probe_fixture::make(mapping.page_size(), payload);
    const auto parsed = ps5rt::parse_elf(file);
    if (!parsed.ok()) throw std::runtime_error("PROBE_PARSE");
    auto built = ps5rt::make_guest_memory(file, *parsed.image);
    if (!built.ok()) throw std::runtime_error("PROBE_LOAD");
    auto& guest = *built.memory;
    const auto& layout = guest.layout();
    if (layout.base_address != probe_fixture::base || layout.regions.size() != 2 ||
        layout.image_size != 2 * mapping.page_size() + 40 || guest.check_execute(layout.entry, 1))
        throw std::runtime_error("PROBE_LAYOUT");
    if (layout.regions[0].virtual_address != layout.base_address || layout.regions[0].flags != 5 ||
        layout.regions[0].memory_size > mapping.page_size() ||
        layout.regions[1].virtual_address != layout.base_address + 2 * mapping.page_size() ||
        layout.regions[1].flags != 6 || layout.regions[1].file_size != 32 || layout.regions[1].memory_size != 40)
        throw std::runtime_error("PROBE_SEGMENT_POLICY");
    for (const auto& region : layout.regions) {
        const auto offset = static_cast<std::size_t>(region.virtual_address - layout.base_address);
        if (offset > mapping.size() || region.memory_size > mapping.size() - offset)
            throw std::runtime_error("PROBE_MAPPING_BOUNDS");
        if (guest.read(region.virtual_address, std::span(mapping.data() + offset,
                static_cast<std::size_t>(region.memory_size)))) throw std::runtime_error("PROBE_COPY");
    }
    // Known fixture only: do not guess/merge arbitrary ELF page permissions.
    mapping.protect_page(0, 5);
    mapping.protect_page(1, 0);
    mapping.protect_page(2, 6);
    return std::move(*built.memory);
}

int loaded_experiment(std::string_view mode) {
    Pages mapping(3);
    auto guest = prepare_loaded_mapping(mapping);
    const auto& layout = guest.layout();
    const auto entry_offset = static_cast<std::size_t>(layout.entry - layout.base_address);
    const auto data_offset = 2 * mapping.page_size();
    std::array<std::uint64_t, 5> before{};
    std::memcpy(before.data(), mapping.data() + data_offset, sizeof(before));
    if (before[4] != 0) throw std::runtime_error("PROBE_BSS");
    std::cout << "{\"phase\":\"ready\",\"fixture\":\"" << mode
              << "\",\"external_binary_execution\":false}\n" << std::flush;
    if (mode == "loaded-gap") {
        const auto observed = *static_cast<volatile std::uint8_t*>(mapping.data() + mapping.page_size());
        (void)observed;
        return 1;
    }
    const auto returned = invoke_leaf(mapping.data() + entry_offset);
    std::array<std::uint64_t, 5> after{};
    std::memcpy(after.data(), mapping.data() + data_offset, sizeof(after));
    if (returned != 127 || after[0] != 5 || after[1] != 9 || after[2] != 42 || after[3] != 0x55 || after[4] != 0)
        throw std::runtime_error("PROBE_LOADED_ORACLE");
    // Logical backing and native pages are distinct. Synchronize this known
    // result range explicitly; this is not automatic tracking of guest writes.
    if (guest.write(layout.base_address + data_offset + 16,
            std::span(mapping.data() + data_offset + 16, sizeof(std::uint64_t))))
        throw std::runtime_error("PROBE_WRITEBACK");
    std::array<std::uint8_t, 8> stored{};
    const std::array<std::uint8_t, 8> expected_stored{42, 0, 0, 0, 0, 0, 0, 0};
    if (guest.read(layout.base_address + data_offset + 16, stored) || stored != expected_stored)
        throw std::runtime_error("PROBE_READBACK");
    std::cout << "{\"phase\":\"result\",\"fixture\":\"loaded-elf\",\"returned\":127,\"stored\":42,"
                 "\"bss_zero\":true,\"ps5_execution_supported\":false,\"contract\":\"host-leaf-function-v1\"}\n";
    return 0;
}

int stack_experiment(std::string_view mode) {
    const bool fault = mode == "guest-stack-guard";
    Pages mapping(3);
    const auto payload = stack_fixture::entry(fault);
    auto guest = prepare_loaded_mapping(mapping, payload);
    Pages stack(16);
    Pages bridge;
    Pages observations;
    const auto bridge_bytes = stack_fixture::bridge();
    if (bridge_bytes.size() > bridge.size()) throw std::runtime_error("PROBE_BRIDGE_SIZE");
    std::memcpy(bridge.data(), bridge_bytes.data(), bridge_bytes.size());
    bridge.seal();
    const auto entry_offset = static_cast<std::size_t>(guest.layout().entry - guest.layout().base_address);
    const std::vector<std::vector<std::string>> cases{
        {}, {""}, {"probe", "alpha"}, {"one", "two", "three"}, {std::string("\x80\xff", 2), ""},
        {std::string(257, 'x'), "tail"}
    };
    bool first = true;
    for (const auto& arguments : cases) {
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(stack.data()));
        const auto built = ps5rt::build_startup_stack(base, stack.size(), arguments);
        if (!built.ok()) throw std::runtime_error("PROBE_STARTUP_BUILD");
        const auto& startup = *built.stack;
        const auto offset = static_cast<std::size_t>(startup.frame_address - base);
        std::memcpy(stack.data(), startup.bytes.data(), startup.bytes.size());
        std::memset(observations.data(), 0, observations.size());
        const auto guard = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(stack.guard()));
        std::memcpy(observations.data() + 192, &guard, sizeof(guard));
        if (first) std::cout << "{\"phase\":\"ready\",\"fixture\":\"" << mode
                             << "\",\"external_binary_execution\":false}\n" << std::flush;
        const auto checksum = invoke_stack(bridge.data(), mapping.data() + entry_offset,
                                           stack.data() + offset, observations.data());
        if (fault) return 1; // Expected fault did not occur.
        std::array<std::uint64_t, 25> seen{};
        std::memcpy(seen.data(), observations.data(), sizeof(seen));
        std::uint64_t expected = arguments.size();
        for (const auto& argument : arguments)
            for (const char byte : argument) expected += static_cast<unsigned char>(byte);
        const bool registers = std::equal(seen.begin() + 8, seen.begin() + 16, seen.begin() + 16);
        if (checksum != expected || seen[0] != startup.frame_address - 8 || seen[1] != arguments.size() ||
            seen[2] != startup.argv_address || seen[3] != 8 || seen[4] != seen[0] - 16 || seen[4] % 16 != 8 ||
            seen[5] == 0 || seen[5] != seen[6] || !registers ||
            std::memcmp(stack.data() + offset, startup.bytes.data() + offset, stack.size() - offset) != 0 ||
            std::memcmp(stack.data(), startup.bytes.data(), 128) != 0)
            throw std::runtime_error("PROBE_STARTUP_ORACLE");
        if (first) std::cout << "{\"phase\":\"result\",\"fixture\":\"guest-stack\",\"cases\":[";
        else std::cout << ',';
        first = false;
        std::cout << "{\"argc\":" << arguments.size() << ",\"checksum\":" << checksum
                  << ",\"entry_alignment\":8,\"nested_alignment\":8,\"host_stack_restored\":true,"
                     "\"integer_registers_restored\":true,\"arguments_unchanged\":true}";
    }
    std::cout << "],\"contract\":\"synthetic-stack-v1\",\"ps5_execution_supported\":false}\n";
    return 0;
}

int experiment(std::string_view mode) {
    Pages code;
    Pages state;
    const auto bytes = program(mode);
    std::memcpy(code.data(), bytes.data(), bytes.size());
    if (mode != "non-executable") code.seal();
    std::cout << "{\"phase\":\"ready\",\"fixture\":\"" << mode
              << "\",\"external_binary_execution\":false}\n" << std::flush;
    if (mode != "arithmetic") {
        (void)invoke(code.data(), mode == "guard-read" ? state.guard() : state.data());
        std::cerr << "PROBE_EXPECTED_FAULT_OR_TIMEOUT_DID_NOT_OCCUR\n";
        return 1;
    }

    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::array<std::uint64_t, 3>> inputs{
        {0, 0, 0}, {5, 9, 0}, {max, 1, 0}, {max, max, max},
        {0x8000000000000000ULL, 0, 0x55aa55aa55aa55aaULL},
        {0x7fffffffffffffffULL, 1, 0xa5a5a5a5a5a5a5a5ULL}
    };
    std::uint64_t seed = 0x123456789abcdef0ULL;
    const auto next = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return seed;
    };
    for (std::size_t i = 0; i < 256; ++i) inputs.push_back({next(), next(), next()});
    std::cout << "{\"phase\":\"result\",\"fixture\":\"arithmetic\",\"cases\":[";
    bool first = true;
    for (const auto& input : inputs) {
        const std::array<std::uint64_t, 4> initial{input[0], input[1], 0xdeadbeefcafebabeULL, input[2]};
        std::memset(state.data(), 0xa5, state.size());
        std::memcpy(state.data() + 8, initial.data(), sizeof(initial));
        std::vector<std::uint8_t> expected(state.data(), state.data() + state.size());
        const auto product = (input[0] + input[1]) * std::uint64_t{3};
        std::memcpy(expected.data() + 8 + 16, &product, sizeof(product));
        const auto returned = invoke(code.data(), state.data() + 8);
        std::array<std::uint64_t, 4> observed{};
        std::memcpy(observed.data(), state.data() + 8, sizeof(observed));
        if (returned != (product ^ input[2]) || std::memcmp(state.data(), expected.data(), state.size()) != 0 ||
            std::memcmp(code.data(), bytes.data(), bytes.size()) != 0) {
            std::cerr << "PROBE_ORACLE_MISMATCH\n";
            return 1;
        }
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"a\":"; hex_value(input[0]);
        std::cout << ",\"b\":"; hex_value(input[1]);
        std::cout << ",\"mask\":"; hex_value(input[2]);
        std::cout << ",\"stored\":"; hex_value(observed[2]);
        std::cout << ",\"returned\":"; hex_value(returned);
        std::cout << '}';
    }
    std::cout << "],\"case_count\":" << inputs.size()
              << ",\"memory_checks\":true,\"ps5_execution_supported\":false}\n";
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) { std::cerr << "Usage: ps5rt_native_probe <fixed-fixture-name>\n"; return 2; }
    const std::string_view mode(argv[1]);
    if (mode != "arithmetic" && mode != "illegal-instruction" && mode != "write-code" &&
        mode != "guard-read" && mode != "non-executable" && mode != "hang" &&
        mode != "loaded-elf" && mode != "loaded-gap" && mode != "guest-stack" && mode != "guest-stack-guard") {
        std::cerr << "PROBE_UNKNOWN_FIXTURE: files and arbitrary bytes are not accepted by this experiment\n";
        return 2;
    }
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#else
    const rlimit limit{0, 0};
    if (setrlimit(RLIMIT_CORE, &limit) != 0) { std::cerr << "PROBE_CORE_LIMIT\n"; return 3; }
#endif
    try {
        if (mode == "guest-stack" || mode == "guest-stack-guard") return stack_experiment(mode);
        if (mode == "loaded-elf" || mode == "loaded-gap") return loaded_experiment(mode);
        return experiment(mode);
    }
    catch (const std::exception&) { std::cerr << "PROBE_HOST_SETUP_FAILURE\n"; return 3; }
}
