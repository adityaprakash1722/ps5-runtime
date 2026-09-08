#include "ps5rt/loader.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace ps5rt {
namespace {

bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t extent) {
    return offset <= extent && size <= extent - offset;
}

bool power_of_two(std::uint64_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

void error(PlanResult& result, const char* code, const char* message,
           std::optional<std::uint64_t> offset = {}) {
    result.diagnostics.push_back({Severity::error, code, message, offset});
}

} // namespace

PlanResult make_load_plan(const ElfImage& elf, std::uint64_t file_size,
                          const LoaderLimits& limits) {
    PlanResult result;
    if (elf.header.machine != 62) {
        error(result, "LOADER_MACHINE", "Offline loading currently requires x86-64 (machine 62).");
        return result;
    }
    if (elf.header.type != 2 && elf.header.type != 3) {
        error(result, "LOADER_TYPE",
              "Offline loading currently supports standard ET_EXEC/ET_DYN only; vendor-specific types remain unsupported.");
        return result;
    }
    if (elf.programs.size() > 4096) {
        error(result, "LOADER_LIMIT", "Too many program headers for the first loader milestone.");
        return result;
    }
    LoadPlan plan;
    plan.entry = elf.header.entry;
    std::uint64_t last_address = 0;
    plan.base_address = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < elf.programs.size(); ++i) {
        const auto& p = elf.programs[i];
        if (p.type != 1) continue; // Only PT_LOAD contributes bytes to this offline layout.
        if (p.file_size > p.memory_size) {
            error(result, "LOADER_SEGMENT_SIZE", "PT_LOAD file size exceeds memory size.", p.offset);
            return result;
        }
        if (p.file_size != 0 && !fits(p.offset, p.file_size, file_size)) {
            error(result, "LOADER_FILE_BOUNDS", "PT_LOAD file bytes lie outside the supplied file.", p.offset);
            return result;
        }
        if (p.memory_size > std::numeric_limits<std::uint64_t>::max() - p.virtual_address) {
            error(result, "LOADER_ADDRESS_OVERFLOW", "PT_LOAD virtual-address range overflows.", p.offset);
            return result;
        }
        if (p.alignment > 1 && (!power_of_two(p.alignment) ||
            p.virtual_address % p.alignment != p.offset % p.alignment)) {
            error(result, "LOADER_ALIGNMENT", "PT_LOAD alignment or address/offset congruence is invalid.", p.offset);
            return result;
        }
        if ((p.flags & ~7U) != 0) {
            error(result, "LOADER_FLAGS", "OS/processor-specific PT_LOAD permissions are not supported.", p.offset);
            return result;
        }
        if (p.memory_size == 0) continue;
        plan.regions.push_back({static_cast<std::uint64_t>(i), p.virtual_address, p.offset,
                                p.file_size, p.memory_size, p.flags, p.alignment});
        plan.base_address = std::min(plan.base_address, p.virtual_address);
        last_address = std::max(last_address, p.virtual_address + p.memory_size);
    }
    if (plan.regions.empty()) {
        error(result, "LOADER_NO_SEGMENTS", "No nonempty PT_LOAD regions were found.");
        return result;
    }
    std::sort(plan.regions.begin(), plan.regions.end(), [](const auto& a, const auto& b) {
        return a.virtual_address < b.virtual_address;
    });
    for (std::size_t i = 1; i < plan.regions.size(); ++i) {
        const auto& previous = plan.regions[i - 1];
        if (plan.regions[i].virtual_address < previous.virtual_address + previous.memory_size) {
            error(result, "LOADER_OVERLAP",
                  "Overlapping PT_LOAD byte ranges are unsupported by the offline loader policy.");
            return result;
        }
    }
    plan.image_size = last_address - plan.base_address;
    if (plan.image_size > limits.max_image_bytes ||
        plan.image_size > std::numeric_limits<std::size_t>::max()) {
        error(result, "LOADER_IMAGE_LIMIT", "Offline image span exceeds the configured memory limit.");
        return result;
    }
    if (plan.entry != 0) {
        const bool backed_executable_entry = std::any_of(
            plan.regions.begin(), plan.regions.end(), [&plan](const auto& r) {
                return (r.flags & 1U) != 0 && plan.entry >= r.virtual_address &&
                       plan.entry - r.virtual_address < r.file_size;
            });
        if (!backed_executable_entry) {
            error(result, "LOADER_ENTRY",
                  "Nonzero entry point must lie in file-backed executable PT_LOAD bytes.");
            return result;
        }
    } else {
        result.diagnostics.push_back({Severity::warning, "LOADER_NO_ENTRY",
                                      "No entry point is indicated. This is an offline byte layout only.", {}});
    }
    result.diagnostics.push_back({Severity::warning, "LOADER_OFFLINE_ONLY",
        "No guest code is executed. Relocations, imports, TLS, host page permissions, and process startup are not implemented.", {}});
    if (elf.header.type == 3) {
        result.diagnostics.push_back({Severity::warning, "LOADER_UNRELOCATED",
            "ET_DYN addresses are retained at zero load bias; this image is not relocated or runnable.", {}});
    }
    result.plan = std::move(plan);
    return result;
}

MaterializeResult materialize_image(std::span<const std::uint8_t> file,
                                    const ElfImage& elf, const LoaderLimits& limits) {
    auto planned = make_load_plan(elf, static_cast<std::uint64_t>(file.size()), limits);
    MaterializeResult result;
    result.diagnostics = std::move(planned.diagnostics);
    if (!planned.ok()) return result;
    MemoryImage image;
    image.plan = std::move(*planned.plan);
    // This vector is ordinary data, not mapped executable guest memory. Zeroing
    // includes PT_LOAD's memory-only tail and gaps in this offline representation.
    image.bytes.resize(static_cast<std::size_t>(image.plan.image_size), 0);
    for (const auto& r : image.plan.regions) {
        if (r.file_size == 0) continue; // An empty file extent requires no pointer.
        const auto source = static_cast<std::size_t>(r.file_offset);
        const auto destination = static_cast<std::size_t>(r.virtual_address - image.plan.base_address);
        std::copy_n(file.data() + source, static_cast<std::size_t>(r.file_size),
                    image.bytes.data() + destination);
    }
    result.image = std::move(image);
    return result;
}

} // namespace ps5rt
