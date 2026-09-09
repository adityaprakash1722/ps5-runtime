#pragma once
#include "ps5rt/elf.hpp"

namespace ps5rt {
struct StartupStack {
    std::uint64_t base_address = 0;
    std::uint64_t frame_address = 0; // Pre-CALL RSP, aligned to 16 bytes.
    std::uint64_t argv_address = 0;
    std::uint64_t argc = 0;
    std::vector<std::uint8_t> bytes;
};
struct StartupResult {
    std::optional<StartupStack> stack;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept { return stack.has_value(); }
};

// Synthetic contract only: [argc, argv pointers..., NULL], then NUL-terminated
// argument bytes. No envp, auxiliary vector, TLS, or platform process startup.
// Reserves >=512 bytes BELOW the frame for calls/temporary stack storage.
[[nodiscard]] StartupResult build_startup_stack(std::uint64_t base_address,
                                               std::uint64_t stack_size,
                                               std::span<const std::string> arguments);
} // namespace ps5rt
