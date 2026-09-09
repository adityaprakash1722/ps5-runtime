#include "ps5rt/startup.hpp"
#include <cstring>
#include <limits>
#include <utility>

namespace ps5rt {
StartupResult build_startup_stack(std::uint64_t base, std::uint64_t size,
                                  std::span<const std::string> arguments) {
    StartupResult result;
    const auto fail = [&](const char* code, const char* message) {
        result.diagnostics.push_back({Severity::error, code, message, {}});
        return result;
    };
    if (base % 16 != 0 || size % 16 != 0)
        return fail("STARTUP_ALIGNMENT", "Stack base and size must be multiples of 16.");
    if (size < 4096 || size > 1024 * 1024)
        return fail("STARTUP_SIZE", "Synthetic stack size must be between 4 KiB and 1 MiB.");
    if (size > std::numeric_limits<std::uint64_t>::max() - base)
        return fail("STARTUP_OVERFLOW", "Stack address range overflows.");
    if (arguments.size() > 64)
        return fail("STARTUP_ARGUMENT_COUNT", "Synthetic startup supports at most 64 arguments.");
    std::uint64_t string_bytes = 0;
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos)
            return fail("STARTUP_ARGUMENT_NUL", "Arguments cannot contain embedded NUL bytes.");
        if (argument.size() >= 16384 || argument.size() + 1 > 16384 - string_bytes)
            return fail("STARTUP_ARGUMENT_BYTES", "Arguments including terminators exceed 16 KiB.");
        string_bytes += argument.size() + 1;
    }
    const auto frame_bytes = (static_cast<std::uint64_t>(arguments.size()) + 2) * 8;
    if (string_bytes > size || frame_bytes > size - string_bytes)
        return fail("STARTUP_SPACE", "Stack cannot hold the argument frame and strings.");
    const auto frame_offset = (size - string_bytes - frame_bytes) & ~std::uint64_t{15};
    if (frame_offset < 512)
        return fail("STARTUP_HEADROOM", "Stack requires at least 512 bytes below its startup frame.");
    StartupStack stack;
    stack.base_address = base;
    stack.frame_address = base + frame_offset;
    stack.argv_address = stack.frame_address + 8;
    stack.argc = arguments.size();
    stack.bytes.resize(static_cast<std::size_t>(size), 0);
    const auto put = [&](std::uint64_t offset, std::uint64_t value) {
        for (std::size_t i = 0; i < 8; ++i)
            stack.bytes[static_cast<std::size_t>(offset) + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    put(frame_offset, stack.argc);
    auto cursor = size - string_bytes;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        put(frame_offset + 8 + i * 8, base + cursor);
        const auto& argument = arguments[i];
        if (!argument.empty()) std::memcpy(stack.bytes.data() + static_cast<std::size_t>(cursor),
                                          argument.data(), argument.size());
        cursor += argument.size() + 1;
    }
    // argv[argc] and string terminators retain their initialized zero bytes.
    result.stack = std::move(stack);
    return result;
}
} // namespace ps5rt
