#pragma once

#include "ps5rt/elf.hpp"

namespace ps5rt {

struct LoaderLimits {
    std::uint64_t max_image_bytes = 64ULL * 1024 * 1024;
};

struct LoadRegion {
    std::uint64_t program_index = 0;
    std::uint64_t virtual_address = 0;
    std::uint64_t file_offset = 0;
    std::uint64_t file_size = 0;
    std::uint64_t memory_size = 0;
    std::uint32_t flags = 0;
    std::uint64_t alignment = 0;
};

struct LoadPlan {
    std::uint64_t base_address = 0;
    std::uint64_t image_size = 0;
    std::uint64_t entry = 0;
    std::vector<LoadRegion> regions;
};

struct PlanResult {
    std::optional<LoadPlan> plan;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept { return plan.has_value(); }
};

struct MemoryImage {
    LoadPlan plan;
    std::vector<std::uint8_t> bytes;
};

struct MaterializeResult {
    std::optional<MemoryImage> image;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept { return image.has_value(); }
};

// Offline byte layout only: no native virtual-address allocation or execution.
// The first milestone accepts only standard ET_EXEC/ET_DYN, machine x86-64.
[[nodiscard]] PlanResult make_load_plan(const ElfImage& elf, std::uint64_t file_size,
                                       const LoaderLimits& limits = {});
[[nodiscard]] MaterializeResult materialize_image(std::span<const std::uint8_t> file,
                                                  const ElfImage& elf,
                                                  const LoaderLimits& limits = {});

} // namespace ps5rt
