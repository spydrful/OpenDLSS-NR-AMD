#pragma once
// Declared Workgroup storage for the selected AMD attention shaders. These
// checks precede pipeline creation; a driver accepting an over-limit shader
// does not waive VkPhysicalDeviceLimits::maxComputeSharedMemorySize.
#include <cstdint>
#include <stdexcept>
#include <string>
#include "amd_config.h"

namespace amd {
inline constexpr uint32_t kLegacyWindowLdsBytes =
    3u * 2048u + 4096u + 2u * 4096u + 4u * 4096u;
inline constexpr uint32_t kCompact64WindowLdsBytes =
    3u * 2048u + 4096u + 2u * 4096u + 4u * 1024u;

inline uint32_t windowLdsBytes(bool optimized, uint32_t queryRows,
                               WindowLayout layout = WindowLayout::Staged) {
  if (layout != WindowLayout::Staged && layout != WindowLayout::Register &&
      layout != WindowLayout::RegisterRte && layout != WindowLayout::ArenaRte)
    throw std::runtime_error("invalid AMD attention layout");
  if (layout != WindowLayout::Staged && (!optimized || (queryRows != 16 && queryRows != 32)))
    throw std::runtime_error("register AMD attention requires optimized Q16 or Q32");
  if (!optimized) return kLegacyWindowLdsBytes;
  if (queryRows == 64) return kCompact64WindowLdsBytes;
  if (layout == WindowLayout::ArenaRte)
    return 2u * 2048u + queryRows * (32u + 128u);
  if (queryRows == 16 || queryRows == 32)
    return queryRows * 32u + 2u * 2048u + queryRows * 64u +
        2u * queryRows * 64u + (layout != WindowLayout::Staged ? 2u : 4u) * 1024u;
  throw std::runtime_error("invalid compact AMD attention query count");
}

inline void requireWindowLds(bool optimized, uint32_t queryRows,
                             uint32_t availableBytes, const char* label,
                             WindowLayout layout = WindowLayout::Staged) {
  const uint32_t requiredBytes = windowLdsBytes(optimized, queryRows, layout);
  if (requiredBytes > availableBytes)
    throw std::runtime_error(std::string(label) + " requires " +
        std::to_string(requiredBytes) + " bytes shared memory; device provides " +
        std::to_string(availableBytes) + " bytes");
}
} // namespace amd
