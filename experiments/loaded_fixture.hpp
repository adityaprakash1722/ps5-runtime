#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <span>
#include <vector>

namespace probe_fixture {
constexpr std::uint64_t base = 0x400000;
inline void put(std::vector<std::uint8_t>& file, std::size_t offset, std::uint64_t value, std::size_t width) {
    if (width > 8 || offset > file.size() || width > file.size() - offset)
        throw std::runtime_error("FIXTURE_WRITE_BOUNDS");
    for (std::size_t i = 0; i < width; ++i) file.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

// Default: position-independent, no-argument leaf function under the HOST ABI,
// with RIP-relative operands. An optional original test payload can replace it.
// Neither payload is evidence of a platform process-entry contract.
inline std::vector<std::uint8_t> make(std::uint64_t page_size, std::span<const std::uint8_t> payload = {}) {
    if (page_size < 64 || page_size > 64 * 1024 * 1024)
        throw std::runtime_error("FIXTURE_PAGE_SIZE");
    const auto data_address = base + 2 * page_size;
    std::vector<std::uint8_t> code{0xf3, 0x0f, 0x1e, 0xfa};
    const auto relative = [&](std::uint8_t opcode, std::uint64_t target) {
        code.insert(code.end(), {0x48, opcode, 0x05});
        const auto displacement = target - (base + code.size() + 4);
        if (displacement > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            throw std::runtime_error("FIXTURE_DISPLACEMENT");
        for (std::size_t i = 0; i < 4; ++i) code.push_back(static_cast<std::uint8_t>(displacement >> (8 * i)));
    };
    relative(0x8b, data_address);       // mov rax, [rip + a]
    relative(0x03, data_address + 8);   // add rax, [rip + b]
    code.insert(code.end(), {0x48, 0x6b, 0xc0, 3}); // imul rax, rax, 3
    relative(0x89, data_address + 16);  // mov [rip + stored], rax
    relative(0x33, data_address + 24);  // xor rax, [rip + mask]
    code.push_back(0xc3);
    if (!payload.empty()) code.assign(payload.begin(), payload.end());
    if (code.size() > 0x100 || code.size() > page_size) throw std::runtime_error("FIXTURE_CODE_SIZE");
    std::vector<std::uint8_t> file(0x220, 0);
    file[0] = 0x7f; file[1] = 'E'; file[2] = 'L'; file[3] = 'F';
    file[4] = 2; file[5] = 1; file[6] = 1;
    put(file, 16, 3, 2); put(file, 18, 62, 2); put(file, 20, 1, 4);
    put(file, 24, base, 8); put(file, 32, 64, 8);
    put(file, 52, 64, 2); put(file, 54, 56, 2); put(file, 56, 2, 2);
    const auto segment = [&](std::size_t at, std::uint32_t flags, std::uint64_t offset,
                             std::uint64_t address, std::uint64_t file_size, std::uint64_t memory_size) {
        put(file, at, 1, 4); put(file, at + 4, flags, 4); put(file, at + 8, offset, 8);
        put(file, at + 16, address, 8); put(file, at + 32, file_size, 8);
        put(file, at + 40, memory_size, 8); put(file, at + 48, 1, 8);
    };
    segment(64, 5, 0x100, base, code.size(), code.size());
    segment(120, 6, 0x200, data_address, 32, 40);
    for (std::size_t i = 0; i < code.size(); ++i) file.at(0x100 + i) = code[i];
    put(file, 0x200, 5, 8); put(file, 0x208, 9, 8);
    put(file, 0x210, 0xdeadbeefcafebabeULL, 8); put(file, 0x218, 0x55, 8);
    return file;
}
} // namespace probe_fixture
