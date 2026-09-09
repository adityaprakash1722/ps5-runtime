#pragma once
#include "ps5rt/loader.hpp"

namespace ps5rt {
struct MemoryBuildResult;

// Logical guest addresses are never treated as host pointers. Bytes in gaps
// exist in the offline backing store but are not accessible guest memory.
class GuestMemory {
public:
    [[nodiscard]] const LoadPlan& layout() const noexcept { return image_.plan; }
    // No diagnostic means success. A failed write changes no bytes.
    [[nodiscard]] std::optional<Diagnostic> read(std::uint64_t address, std::span<std::uint8_t> output) const;
    [[nodiscard]] std::optional<Diagnostic> write(std::uint64_t address, std::span<const std::uint8_t> input);
    [[nodiscard]] std::optional<Diagnostic> check_execute(std::uint64_t address, std::uint64_t size) const;
private:
    explicit GuestMemory(MemoryImage image);
    [[nodiscard]] std::optional<Diagnostic> check(std::uint64_t address, std::uint64_t size, std::uint32_t permission) const;
    MemoryImage image_;
    friend MemoryBuildResult make_guest_memory(std::span<const std::uint8_t>, const ElfImage&, const LoaderLimits&);
};

struct MemoryBuildResult {
    std::optional<GuestMemory> memory;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept { return memory.has_value(); }
};

[[nodiscard]] MemoryBuildResult make_guest_memory(std::span<const std::uint8_t> file,
                                                 const ElfImage& elf, const LoaderLimits& limits = {});
} // namespace ps5rt
