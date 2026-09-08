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
    Pages() {
#ifdef _WIN32
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page_ = info.dwPageSize;
#else
        const auto size = sysconf(_SC_PAGESIZE);
        if (size <= 0) throw std::runtime_error("PROBE_PAGE_SIZE");
        page_ = static_cast<std::size_t>(size);
#endif
        if (page_ < 64 || page_ > std::numeric_limits<std::size_t>::max() / 3)
            throw std::runtime_error("PROBE_PAGE_SIZE");
#ifdef _WIN32
        base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page_ * 3,
            MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        if (!base_) throw std::runtime_error("PROBE_ALLOCATE");
        DWORD old = 0;
        if (!VirtualProtect(data(), page_, PAGE_READWRITE, &old)) {
            release();
            throw std::runtime_error("PROBE_WRITABLE");
        }
#else
        auto* mapped = mmap(nullptr, page_ * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) throw std::runtime_error("PROBE_ALLOCATE");
        base_ = static_cast<std::uint8_t*>(mapped);
        if (mprotect(data(), page_, PROT_READ | PROT_WRITE) != 0) {
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
    std::size_t size() const { return page_; }
    void seal() {
#ifdef _WIN32
        DWORD old = 0;
        if (!VirtualProtect(data(), page_, PAGE_EXECUTE_READ, &old))
            throw std::runtime_error("PROBE_EXECUTABLE");
        if (!FlushInstructionCache(GetCurrentProcess(), data(), page_))
            throw std::runtime_error("PROBE_CACHE");
#else
        if (mprotect(data(), page_, PROT_READ | PROT_EXEC) != 0)
            throw std::runtime_error("PROBE_EXECUTABLE");
        __builtin___clear_cache(reinterpret_cast<char*>(data()), reinterpret_cast<char*>(data() + page_));
#endif
    }
private:
    void release() noexcept {
        if (!base_) return;
#ifdef _WIN32
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        munmap(base_, page_ * 3);
#endif
        base_ = nullptr;
    }
    std::uint8_t* base_ = nullptr;
    std::size_t page_ = 0;
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
        mode != "guard-read" && mode != "non-executable" && mode != "hang") {
        std::cerr << "PROBE_UNKNOWN_FIXTURE: files and arbitrary bytes are not accepted by this experiment\n";
        return 2;
    }
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#else
    const rlimit limit{0, 0};
    if (setrlimit(RLIMIT_CORE, &limit) != 0) { std::cerr << "PROBE_CORE_LIMIT\n"; return 3; }
#endif
    try { return experiment(mode); }
    catch (const std::exception&) { std::cerr << "PROBE_HOST_SETUP_FAILURE\n"; return 3; }
}
