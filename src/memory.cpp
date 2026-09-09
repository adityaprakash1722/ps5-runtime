#include "ps5rt/memory.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace ps5rt {
namespace {
Diagnostic failure(const char* code, const char* message) {
    // Diagnostic.offset is a FILE offset elsewhere, so do not put a guest
    // address in it. The caller already holds the failing guest address.
    return {Severity::error, code, message, {}};
}
} // namespace

GuestMemory::GuestMemory(MemoryImage image) : image_(std::move(image)) {}

std::optional<Diagnostic> GuestMemory::check(std::uint64_t address, std::uint64_t size,
                                            std::uint32_t permission) const {
    if (size == 0) return {}; // Explicit no-op; validates no address or permissions.
    if (size > std::numeric_limits<std::uint64_t>::max() - address)
        return failure("MEMORY_OVERFLOW", "Guest address range wraps around.");
    const auto end = address + size;
    auto cursor = address;
    for (const auto& region : image_.plan.regions) {
        const auto region_end = region.virtual_address + region.memory_size; // Validated by loader.
        if (region_end <= cursor) continue;
        if (region.virtual_address > cursor) break;
        if ((region.flags & permission) != permission)
            return failure("MEMORY_PERMISSION", "Guest access is not permitted by the segment flags.");
        cursor = std::min(end, region_end);
        if (cursor == end) return {};
    }
    return failure("MEMORY_UNMAPPED", "Guest range includes memory outside loadable segments.");
}

std::optional<Diagnostic> GuestMemory::read(std::uint64_t address, std::span<std::uint8_t> output) const {
    if (auto error = check(address, output.size(), 4)) return error;
    if (!output.empty()) {
        const auto offset = static_cast<std::size_t>(address - image_.plan.base_address);
        std::copy_n(image_.bytes.data() + offset, output.size(), output.data());
    }
    return {};
}

std::optional<Diagnostic> GuestMemory::write(std::uint64_t address, std::span<const std::uint8_t> input) {
    if (auto error = check(address, input.size(), 2)) return error;
    if (!input.empty()) {
        const auto offset = static_cast<std::size_t>(address - image_.plan.base_address);
        std::copy_n(input.data(), input.size(), image_.bytes.data() + offset);
    }
    return {};
}

std::optional<Diagnostic> GuestMemory::check_execute(std::uint64_t address, std::uint64_t size) const {
    return check(address, size, 1);
}

MemoryBuildResult make_guest_memory(std::span<const std::uint8_t> file, const ElfImage& elf,
                                    const LoaderLimits& limits) {
    auto materialized = materialize_image(file, elf, limits);
    MemoryBuildResult result;
    result.diagnostics = std::move(materialized.diagnostics);
    if (materialized.image) result.memory = GuestMemory(std::move(*materialized.image));
    return result;
}
} // namespace ps5rt
