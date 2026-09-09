#pragma once
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace stack_fixture {
using Bytes = std::vector<std::uint8_t>;
inline void emit(Bytes& b, std::initializer_list<std::uint8_t> values) {
    // Append with explicit capacity-managed operations; GCC 13's optimized
    // range-insert path diagnoses an overflow for these small growing fixtures.
    for (const auto value : values) b.push_back(value);
}
inline void immediate(Bytes& b, std::uint64_t value, std::size_t width) {
    if (width > 8) throw std::runtime_error("FIXTURE_IMMEDIATE_WIDTH");
    for (std::size_t i = 0; i < width; ++i) b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
// mov [base+disp32], source, for bases RAX/R11 only (neither needs SIB).
inline void store(Bytes& b, unsigned base, unsigned source, std::uint32_t offset) {
    if ((base != 0 && base != 11) || source > 15) throw std::runtime_error("FIXTURE_REGISTER");
    emit(b, {static_cast<std::uint8_t>(0x48U | (source >= 8 ? 4U : 0U) | (base >= 8 ? 1U : 0U)),
             0x89, static_cast<std::uint8_t>(0x80U | ((source & 7U) << 3) | (base & 7U))});
    immediate(b, offset, 4);
}
inline Bytes bridge() {
    Bytes b{0xf3, 0x0f, 0x1e, 0xfa};
#ifdef _WIN32
    emit(b, {0x49, 0x89, 0xca, 0x49, 0x89, 0xd3, 0x4c, 0x89, 0xc0}); // R10=RCX, R11=RDX, RAX=R8
#else
    emit(b, {0x49, 0x89, 0xfa, 0x49, 0x89, 0xf3, 0x48, 0x89, 0xd0}); // R10=RDI, R11=RSI, RAX=RDX
#endif
    constexpr unsigned saved[]{3, 5, 7, 6, 12, 13, 14, 15};
    store(b, 0, 4, 40); // host entry RSP
    for (unsigned i = 0; i < 8; ++i) store(b, 0, saved[i], 64 + i * 8);
    emit(b, {0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x50});
    // Save integer nonvolatiles and context pointer on HOST stack.
    emit(b, {0x49, 0x89, 0xe4}); // mov r12,rsp; R12 is reserved by our synthetic guest contract.
    emit(b, {0x4c, 0x89, 0xdc}); // mov rsp,r11: validated, 16-aligned argument frame
    emit(b, {0x48, 0x89, 0xc2}); // mov rdx,rax: out-of-band observation record
    emit(b, {0x41, 0xff, 0xd2}); // call r10: entry RSP = frame - 8
    emit(b, {0x4c, 0x89, 0xe4}); // mov rsp,r12: return to HOST stack
    emit(b, {0x41, 0x5b}); // pop r11: observation record, leaves RAX return value untouched
    emit(b, {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5e, 0x5f, 0x5d, 0x5b});
    store(b, 11, 4, 48); // restored host RSP
    for (unsigned i = 0; i < 8; ++i) store(b, 11, saved[i], 128 + i * 8);
    emit(b, {0xc3});
    return b;
}

inline Bytes entry(bool fault) {
    Bytes b{0xf3, 0x0f, 0x1e, 0xfa};
    if (fault) {
        emit(b, {0x48, 0x8b, 0x82, 0xc0, 0, 0, 0, 0x48, 0x8b, 0x00, 0xc3});
        // Read lower stack guard from record[24], while running on guest stack.
        return b;
    }
    emit(b, {0x48, 0x89, 0x22}); // record[0] = entry RSP
    emit(b, {0x48, 0x8b, 0x7c, 0x24, 0x08}); // mov rdi,[rsp+8]: argc
    emit(b, {0x48, 0x8d, 0x74, 0x24, 0x10}); // lea rsi,[rsp+16]: argv
    emit(b, {0x48, 0x89, 0x7a, 0x08, 0x48, 0x89, 0x72, 0x10}); // observed argc/argv
    emit(b, {0x48, 0x89, 0xe0, 0x83, 0xe0, 0x0f, 0x48, 0x89, 0x42, 0x18}); // entry RSP % 16
    // Deliberately clobber guest-volatile integers; the bridge must restore their HOST values.
    for (const unsigned reg : {3U, 5U, 13U, 14U, 15U}) {
        emit(b, {static_cast<std::uint8_t>(reg >= 8 ? 0x49 : 0x48), static_cast<std::uint8_t>(0xb8U + (reg & 7U))});
        immediate(b, 0x1122334455667700ULL + reg, 8);
    }
    emit(b, {0x48, 0x83, 0xec, 8, 0xe8}); // align before nested call
    const auto call_disp = b.size(); immediate(b, 0, 4);
    emit(b, {0x48, 0x83, 0xc4, 8, 0xc3});
    const auto helper = b.size();
    const auto delta = helper - (call_disp + 4);
    for (std::size_t i = 0; i < 4; ++i) b[call_disp + i] = static_cast<std::uint8_t>(delta >> (8 * i));
    emit(b, {0x48, 0x89, 0x62, 0x20, 0x49, 0x89, 0xf8, 0x31, 0xc9}); // nested RSP; sum=argc; i=0
    const auto arg_loop = b.size();
    emit(b, {0x48, 0x39, 0xf9, 0x73, 0}); // cmp rcx,rdi; jae done
    const auto done_jump = b.size() - 1;
    emit(b, {0x4c, 0x8b, 0x0c, 0xce}); // mov r9,[rsi+rcx*8]
    const auto char_loop = b.size();
    emit(b, {0x41, 0x0f, 0xb6, 0x01, 0x84, 0xc0, 0x74, 0}); // byte -> eax; test; jz next arg
    const auto next_jump = b.size() - 1;
    emit(b, {0x49, 0x01, 0xc0, 0x49, 0xff, 0xc1, 0xeb, 0}); // sum+=byte; ++ptr; loop
    const auto char_jump = b.size() - 1;
    const auto next_arg = b.size();
    emit(b, {0x48, 0xff, 0xc1, 0xeb, 0}); // ++i; loop
    const auto arg_jump = b.size() - 1;
    const auto done = b.size();
    emit(b, {0x4c, 0x89, 0xc0, 0xc3}); // mov rax,r8; ret
    const auto branch = [&](std::size_t at, std::size_t target) {
        const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at + 1);
        if (displacement < -128 || displacement > 127) throw std::runtime_error("FIXTURE_BRANCH");
        b[at] = static_cast<std::uint8_t>(displacement);
    };
    branch(done_jump, done); branch(next_jump, next_arg); branch(char_jump, char_loop); branch(arg_jump, arg_loop);
    return b;
}
} // namespace stack_fixture
