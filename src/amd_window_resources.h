#pragma once
// Declared Workgroup storage for the unchanged AMD attention shaders. These
// checks precede pipeline creation; a driver accepting an over-limit shader
// does not waive VkPhysicalDeviceLimits::maxComputeSharedMemorySize.
#include <cstdint>
#include <stdexcept>
#include <string>

namespace amd {
inline constexpr uint32_t kLegacyWindowLdsBytes =
    3u * 2048u + 4096u + 2u * 4096u + 4u * 4096u;
inline constexpr uint32_t kCompact64WindowLdsBytes =
    3u * 2048u + 4096u + 2u * 4096u + 4u * 1024u;

inline uint32_t windowLdsBytes(bool optimized, uint32_t queryRows) {
  if (!optimized) return kLegacyWindowLdsBytes;
  if (queryRows == 64) return kCompact64WindowLdsBytes;
  if (queryRows == 16 || queryRows == 32)
    return queryRows * 32u + 2u * 2048u + queryRows * 64u +
        2u * queryRows * 64u + 4u * 1024u;
  throw std::runtime_error("invalid compact AMD attention query count");
}

inline void requireWindowLds(bool optimized, uint32_t queryRows,
                             uint32_t availableBytes, const char* label) {
  const uint32_t requiredBytes = windowLdsBytes(optimized, queryRows);
  if (requiredBytes > availableBytes)
    throw std::runtime_error(std::string(label) + " requires " +
        std::to_string(requiredBytes) + " bytes shared memory; device provides " +
        std::to_string(availableBytes) + " bytes");
}
} // namespace amd
