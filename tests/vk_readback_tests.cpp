// CPU-only coverage of the production diagnostic readback contract.
#include "../src/vk_readback.h"
#include <cstdio>
#include <stdexcept>

namespace {
unsigned checks = 0;
void expect(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  try {
    const auto before = vk::readback::deviceWritesToTransfer();
    const auto after = vk::readback::transferWritesToHost();
    expect(before.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 && !before.pNext,
           "invalid device-to-copy dependency structure");
    expect((before.srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) &&
           (before.srcAccessMask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
           "compute outputs are not made available to the copy");
    expect((before.srcStageMask & VK_PIPELINE_STAGE_2_TRANSFER_BIT) &&
           (before.srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT),
           "uploaded padding and captured buffers are not made available to the copy");
    expect(before.dstStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
           before.dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT,
           "copy source reads are outside the dependency");
    expect(after.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 && !after.pNext,
           "invalid copy-to-host dependency structure");
    expect(after.srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
           after.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT,
           "staging writes are outside the host dependency");
    expect(after.dstStageMask == VK_PIPELINE_STAGE_2_HOST_BIT &&
           after.dstAccessMask == VK_ACCESS_2_HOST_READ_BIT,
           "mapped CPU reads are outside the dependency");

    constexpr VkDeviceSize maximum = std::numeric_limits<VkDeviceSize>::max();
    expect(vk::readback::validRange(128, 128, 0), "full allocation rejected");
    expect(vk::readback::validRange(128, 64, 64), "exact-end subrange rejected");
    expect(vk::readback::validRange(128, 0, 128), "empty exact-end subrange rejected");
    expect(vk::readback::validRange(0, 0, 0), "empty buffer rejected");
    expect(vk::readback::validRange(maximum, 0, maximum), "empty maximum-offset range rejected");
    expect(!vk::readback::validRange(128, 0, 129), "offset beyond allocation accepted");
    expect(!vk::readback::validRange(128, 129, 0), "oversized allocation accepted");
    expect(!vk::readback::validRange(128, 65, 64), "one-byte overflow accepted");
    expect(!vk::readback::validRange(128, maximum, 1), "wrapped size plus offset accepted");
    expect(!vk::readback::validRange(128, 128, maximum), "wrapped offset plus size accepted");
    expect(!vk::readback::validRange(maximum, 1, maximum), "maximum-plus-one range accepted");
    expect(!vk::readback::validRange(maximum, maximum, 1), "maximum size with nonzero offset accepted");
    expect(vk::readback::validRange(maximum, maximum, 0) == (sizeof(size_t) >= sizeof(VkDeviceSize)),
           "host address range limit differs");
    std::printf("Vulkan readback contract PASS %u CPU checks; GPU executed:false\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Vulkan readback contract FAIL: %s\n", error.what());
    return 1;
  }
}
