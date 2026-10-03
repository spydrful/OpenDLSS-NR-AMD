// Diagnostic buffer readback dependencies. Core code is MIT licensed.
#pragma once
#include <volk.h>
#include <cstddef>
#include <limits>

namespace vk::readback {

// Subtraction after the offset check avoids overflow in offset + size.
inline bool validRange(VkDeviceSize bufferBytes, VkDeviceSize size, VkDeviceSize offset) {
  return offset <= bufferBytes && size <= bufferBytes - offset &&
         size <= std::numeric_limits<size_t>::max();
}

inline VkMemoryBarrier2 deviceWritesToTransfer() {
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  return barrier;
}

inline VkMemoryBarrier2 transferWritesToHost() {
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  return barrier;
}

}  // namespace vk::readback
