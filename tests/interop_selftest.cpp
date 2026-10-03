// Exercises the production bridge helpers on real D3D12/Vulkan queues. The
// default tests need no model; opt-in runtime tests use hash-verified local
// assets. Reuse production helpers so import and fence regressions are visible.
#include "../game/runtime.cpp"
#include "json.h"
#include <dxgi1_6.h>
#include <cstdio>
#include <limits>
#include <vector>
#if defined(NR_HOST_BINDINGS_SELFTEST)
#include "../third_party/optiscaler-host/OptiScaler/dlssnr/submission/CommandListProxy.h"
#endif

namespace {
std::filesystem::path runtimeLibraryPath(const std::filesystem::path& repository) {
  const char* selected = std::getenv("OPEN_NR_RUNTIME_DLL");
  return selected && *selected ? std::filesystem::absolute(selected) : repository / "build/game/OpenNrRuntime.dll";
}
void expect(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
// D3D resource-format conversion uses RTZ, whereas neural half arithmetic
// uses RNE. See Direct3D 11.3 Functional Specification section 3.2.2.
uint16_t d3dHalfStore(float value) {
  expect(std::isfinite(value), "nonfinite D3D half-store oracle input");
  const uint32_t bits = num::f32Bits(value), sign = (bits >> 16) & 0x8000, mantissa = bits & 0x7fffff;
  const int exponent = int((bits >> 23) & 255) - 112;
  if (exponent >= 31) return uint16_t(sign | 0x7bff);
  if (exponent < -10) return uint16_t(sign);
  if (exponent <= 0) return uint16_t(sign | ((mantissa | 0x800000) >> (14 - exponent)));
  return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}
ComPtr<ID3D12Resource> testBuffer(ID3D12Device* device, uint64_t bytes,
                                D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                        nullptr, IID_PPV_ARGS(&resource)), "test buffer");
  return resource;
}
struct D3DCommands {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  void create(ID3D12Device* device) {
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "test allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&list)), "test command list");
    check(list->Close(), "initial command close");
  }
  void begin() {
    check(allocator->Reset(), "test allocator reset");
    check(list->Reset(allocator.Get(), nullptr), "test list reset");
  }
  void submit(ID3D12CommandQueue* queue) {
    check(list->Close(), "test list close");
    ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
  }
};
struct Completion {
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  ~Completion() { if (event) CloseHandle(event); }
  void create(ID3D12Device* device) {
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "test completion fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr); expect(event != nullptr, "test completion event");
  }
  void wait(uint64_t value) {
    if (fence->GetCompletedValue() >= value) return;
    check(fence->SetEventOnCompletion(value, event), "test completion event request");
    expect(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "D3D12/Vulkan roundtrip timed out");
  }
};
uint32_t pattern(uint32_t index, uint32_t seed) {
  return 0xdeadbeefu ^ (index * 0x9e3779b9u) ^ (seed * 0x5bd1e995u);
}
struct AssetEnvironment {
  std::wstring previous;
  bool present = false;
  explicit AssetEnvironment(const std::wstring& value) {
    const DWORD length = GetEnvironmentVariableW(L"OPEN_NR_ASSETS", nullptr, 0);
    if (length) {
      previous.resize(length); GetEnvironmentVariableW(L"OPEN_NR_ASSETS", previous.data(), length);
      previous.resize(wcslen(previous.c_str())); present = true;
    }
    expect(SetEnvironmentVariableW(L"OPEN_NR_ASSETS", value.c_str()) != 0, "test asset environment override failed");
  }
  ~AssetEnvironment() { SetEnvironmentVariableW(L"OPEN_NR_ASSETS", present ? previous.c_str() : nullptr); }
};
uint32_t expectedWord(uint32_t index, uint32_t words, uint32_t seed) {
  const uint32_t marker = 0xf00d0000u | seed;
  if (index < 16) return marker;
  if (index >= words - 4) return marker ^ 0xffffffffu;
  return pattern(index, seed);
}
struct TransferSlot {
  SharedBuffer input, output;
  ComPtr<ID3D12Resource> upload, readback;
  D3DCommands producer, consumer;
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  uint64_t submitted = 0;
  uint32_t seed = 0, words = 0;
  ~TransferSlot() { if (pool) vkDestroyCommandPool(device, pool, nullptr); }
  void create(ID3D12Device* d3d, vk::Context& context, uint32_t count) {
    words = count; device = context.device();
    input.create(d3d, context, uint64_t(words) * 4); output.create(d3d, context, uint64_t(words) * 4);
    upload = testBuffer(d3d, uint64_t(words) * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = testBuffer(d3d, uint64_t(words) * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    producer.create(d3d); consumer.create(d3d);
    VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; p.queueFamilyIndex = context.queueFamily();
    VK_CHECK(vkCreateCommandPool(device, &p, nullptr, &pool));
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool = pool; a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &a, &commands));
  }
  void verify() {
    if (!submitted) return;
    uint32_t* actual = nullptr; D3D12_RANGE read{0, size_t(words) * 4};
    check(readback->Map(0, &read, reinterpret_cast<void**>(&actual)), "test readback map");
    for (uint32_t index = 0; index < words; ++index) {
      const uint32_t expected = expectedWord(index, words, seed);
      if (actual[index] != expected) {
        char text[160]; snprintf(text, sizeof(text), "roundtrip frame %u word %u: got %08x expected %08x",
                                seed, index, actual[index], expected);
        D3D12_RANGE empty{0, 0}; readback->Unmap(0, &empty); throw std::runtime_error(text);
      }
    }
    D3D12_RANGE empty{0, 0}; readback->Unmap(0, &empty); submitted = 0;
  }
};
void transferFrames(ID3D12Device* d3d, ID3D12CommandQueue* queue, vk::Context& context,
                    SharedFence& ready, SharedFence& finished, Completion& completion,
                    uint32_t words, uint32_t frames, uint64_t& value) {
  std::array<TransferSlot, 2> slots;
  for (auto& slot : slots) slot.create(d3d, context, words);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    auto& slot = slots[frame % slots.size()];
    if (slot.submitted) { completion.wait(slot.submitted); slot.verify(); }
    slot.seed = uint32_t(++value);
    uint32_t* mapped = nullptr; D3D12_RANGE empty{0, 0};
    check(slot.upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "test upload map");
    for (uint32_t index = 0; index < words; ++index) mapped[index] = pattern(index, slot.seed);
    D3D12_RANGE written{0, size_t(words) * 4}; slot.upload->Unmap(0, &written);
    slot.producer.begin();
    transition(slot.producer.list.Get(), slot.input.d3d.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    slot.producer.list->CopyBufferRegion(slot.input.d3d.Get(), 0, slot.upload.Get(), 0, uint64_t(words) * 4);
    transition(slot.producer.list.Get(), slot.input.d3d.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    slot.producer.submit(queue); check(queue->Signal(ready.fence.Get(), value), "test producer signal");

    VK_CHECK(vkResetCommandPool(context.device(), slot.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(slot.commands, &begin));
    VkBufferMemoryBarrier2 acquire[2]{};
    for (uint32_t index = 0; index < 2; ++index) {
      auto& b = acquire[index]; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
      b.srcStageMask = VK_PIPELINE_STAGE_2_NONE; b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      b.dstAccessMask = index ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; b.dstQueueFamilyIndex = context.queueFamily();
      b.buffer = index ? slot.output.buffer.buffer : slot.input.buffer.buffer; b.size = VK_WHOLE_SIZE;
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 2; dependency.pBufferMemoryBarriers = acquire;
    vkCmdPipelineBarrier2(slot.commands, &dependency);
    VkBufferCopy copy{0, 0, uint64_t(words) * 4};
    vkCmdCopyBuffer(slot.commands, slot.input.buffer.buffer, slot.output.buffer.buffer, 1, &copy);
    VkMemoryBarrier2 order{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    order.srcStageMask = order.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    order.srcAccessMask = order.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo orderDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    orderDependency.memoryBarrierCount = 1; orderDependency.pMemoryBarriers = &order;
    vkCmdPipelineBarrier2(slot.commands, &orderDependency);
    vkCmdFillBuffer(slot.commands, slot.output.buffer.buffer, 0, 64, 0xf00d0000u | slot.seed);
    vkCmdFillBuffer(slot.commands, slot.output.buffer.buffer, uint64_t(words - 4) * 4, 16, (0xf00d0000u | slot.seed) ^ 0xffffffffu);
    for (auto& b : acquire) {
      b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_NONE; b.dstAccessMask = 0;
      std::swap(b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
    }
    vkCmdPipelineBarrier2(slot.commands, &dependency); VK_CHECK(vkEndCommandBuffer(slot.commands));
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = timeline.pSignalSemaphoreValues = &value;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
    submit.waitSemaphoreCount = submit.signalSemaphoreCount = 1;
    submit.pWaitSemaphores = &ready.semaphore; submit.pSignalSemaphores = &finished.semaphore; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &slot.commands;
    VK_CHECK(vkQueueSubmit(context.queue(), 1, &submit, VK_NULL_HANDLE));
    check(queue->Wait(finished.fence.Get(), value), "test consumer GPU wait");
    slot.consumer.begin();
    transition(slot.consumer.list.Get(), slot.output.d3d.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    slot.consumer.list->CopyBufferRegion(slot.readback.Get(), 0, slot.output.d3d.Get(), 0, uint64_t(words) * 4);
    transition(slot.consumer.list.Get(), slot.output.d3d.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    slot.consumer.submit(queue); check(queue->Signal(completion.fence.Get(), value), "test consumer completion signal");
    slot.submitted = value;
  }
  for (auto& slot : slots) { completion.wait(slot.submitted); slot.verify(); }
  context.waitIdle();
  printf("interop two-slot roundtrip: %u frames x %llu bytes, fence values through %llu, exact PASS\n",
         frames, (unsigned long long)words * 4, (unsigned long long)value);
}
struct TestTexture {
  ComPtr<ID3D12Resource> texture, upload;
  void create(ID3D12Device* device, ID3D12GraphicsCommandList* commands, uint32_t width, uint32_t height,
              DXGI_FORMAT format, uint32_t pixelBytes, const void* data) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&texture)), "pack test texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
    upload = testBuffer(device, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr; D3D12_RANGE empty{0, 0};
    check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "pack test texture upload map");
    for (uint32_t y = 0; y < height; ++y)
      memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
             static_cast<const uint8_t*>(data) + size_t(y) * width * pixelBytes, size_t(width) * pixelBytes);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = texture.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(commands, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
};
void packCases(ID3D12Device* d3d, ID3D12CommandQueue* queue, vk::Context& context,
               SharedFence& ready, SharedFence& finished, Completion& completion,
               const std::filesystem::path& repository, uint64_t& value) {
  PackPipelines pipelines; pipelines.create(d3d, repository / "game/shaders/bridge.hlsl");
  const uint32_t width = 3, height = 2, pixels = width * height;
  using Pixel = std::array<float, 4>;
  std::vector<Pixel> color(7 * 6), motion(4 * 4);
  for (uint32_t y = 0; y < 6; ++y) for (uint32_t x = 0; x < 7; ++x)
    color[y * 7 + x] = {float(x) * .25f, float(y) * .5f, float(x + y) * .125f, 1.f};
  motion[1 * 4 + 1] = {.03125f, -.0625f, 0, 0};
  motion[1 * 4 + 2] = {.5f, 0, 0, 0}; // moves the right columns out of bounds
  motion[2 * 4 + 1] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 0};
  motion[2 * 4 + 2] = {-.015625f, .03125f, 0, 0};
  for (uint32_t test = 0; test < 4; ++test) {
    const float exposure = test == 3 ? -4.f : 4.f;
    PackParams params{width, height, 1, 2, 2, 2, 1, 1, 8.f, 4.f, 2.f, 3.f, .25f, -.25f,
                      uint32_t(test != 1), uint32_t(test != 2)};
    SharedBuffer packed, marker; packed.create(d3d, context, (pixels * 2 + 1) * sizeof(Pixel)); marker.create(d3d, context, (pixels + 1) * sizeof(Pixel));
    auto host = context.createBuffer((pixels * 2 + 1) * sizeof(Pixel), true, "pack test Vulkan readback");
    D3DCommands producer; producer.create(d3d); producer.begin();
    TestTexture colorTexture, motionTexture, exposureTexture;
    colorTexture.create(d3d, producer.list.Get(), 7, 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, color.data());
    motionTexture.create(d3d, producer.list.Get(), 4, 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, motion.data());
    exposureTexture.create(d3d, producer.list.Get(), 1, 1, DXGI_FORMAT_R32_FLOAT, 4, &exposure);
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{}; heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = 5; heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap; check(d3d->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap)), "pack test descriptors");
    const UINT step = d3d->GetDescriptorHandleIncrementSize(heapDescription.Type);
    auto handle = [&](UINT index) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += size_t(index) * step; return h; };
    auto makeSrv = [&](ID3D12Resource* texture, UINT index) {
      D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.Format = texture->GetDesc().Format;
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Texture2D.MipLevels = 1;
      d3d->CreateShaderResourceView(texture, &desc, handle(index));
    };
    makeSrv(colorTexture.texture.Get(), 0); makeSrv(motionTexture.texture.Get(), 1); makeSrv(exposureTexture.texture.Get(), 2);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = pixels * 2 + 1; uav.Buffer.StructureByteStride = sizeof(Pixel);
    d3d->CreateUnorderedAccessView(packed.d3d.Get(), nullptr, &uav, handle(3));
    uav.Buffer.NumElements = pixels + 1; d3d->CreateUnorderedAccessView(marker.d3d.Get(), nullptr, &uav, handle(4));
    transition(producer.list.Get(), packed.d3d.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(producer.list.Get(), marker.d3d.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()}; producer.list->SetDescriptorHeaps(1, heaps);
    producer.list->SetComputeRootSignature(pipelines.root.Get()); producer.list->SetPipelineState(pipelines.pack.Get());
    producer.list->SetComputeRoot32BitConstants(0, 16, &params, 0);
    producer.list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart()); producer.list->Dispatch(1, 1, 1);
    transition(producer.list.Get(), packed.d3d.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    transition(producer.list.Get(), marker.d3d.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    producer.submit(queue); ++value; check(queue->Signal(ready.fence.Get(), value), "pack test input fence");

    VkCommandBuffer commands = context.beginCommands();
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE; barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT; barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = context.queueFamily(); barrier.buffer = packed.buffer.buffer; barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
    VkBufferCopy copy{0, 0, host.size}; vkCmdCopyBuffer(commands, packed.buffer.buffer, host.buffer, 1, &copy);
    VkMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo hostDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; hostDependency.memoryBarrierCount = 1; hostDependency.pMemoryBarriers = &hostBarrier;
    vkCmdPipelineBarrier2(commands, &hostDependency);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE; barrier.dstAccessMask = 0; std::swap(barrier.srcQueueFamilyIndex, barrier.dstQueueFamilyIndex);
    vkCmdPipelineBarrier2(commands, &dependency); VK_CHECK(vkEndCommandBuffer(commands));
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = timeline.pSignalSemaphoreValues = &value;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline}; submit.waitSemaphoreCount = submit.signalSemaphoreCount = 1;
    submit.pWaitSemaphores = &ready.semaphore; submit.pSignalSemaphores = &finished.semaphore; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &commands;
    VK_CHECK(vkQueueSubmit(context.queue(), 1, &submit, VK_NULL_HANDLE));
    check(queue->Wait(finished.fence.Get(), value), "pack test output GPU wait");
    check(queue->Signal(completion.fence.Get(), value), "pack test completion fence"); completion.wait(value);
    auto result = static_cast<const Pixel*>(host.mapped);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
      const uint32_t pixel = y * width + x;
      expect(!memcmp(result[pixel * 2].data(), color[(y + 2) * 7 + x + 1].data(), sizeof(Pixel)), "pack color rectangle differs");
      const uint32_t mx = std::min(((x * 2 + 1) * params.motionWidth) / (width * 2), params.motionWidth - 1);
      const uint32_t my = std::min(((y * 2 + 1) * params.motionHeight) / (height * 2), params.motionHeight - 1);
      const auto& raw = motion[(my + 1) * 4 + mx + 1];
      const float vx = params.temporal ? raw[0] * 8.f / 2.f + .25f / width : 0.f;
      const float vy = params.temporal ? raw[1] * 4.f / 2.f - .25f / height : 0.f;
      const float previousX = (float(x) + .5f) / width + vx, previousY = (float(y) + .5f) / height + vy;
      const bool valid = params.temporal && std::isfinite(vx) && std::isfinite(vy) && previousX >= 0 && previousX <= 1 && previousY >= 0 && previousY <= 1;
      const auto& actual = result[pixel * 2 + 1];
      if (!(std::isnan(vx) ? std::isnan(actual[0]) : std::fabs(actual[0] - vx) <= 1e-6f))
        fprintf(stderr, "pack test %u pixel(%u,%u) raw=(%.9g,%.9g) actual=(%.9g,%.9g,%.9g) expected=(%.9g,%.9g,%u)\n",
                test, x, y, raw[0], raw[1], actual[0], actual[1], actual[2], vx, vy, unsigned(valid));
      expect(std::isnan(vx) ? std::isnan(actual[0]) : std::fabs(actual[0] - vx) <= 1e-6f, "pack motion x differs");
      expect(std::isnan(vy) ? std::isnan(actual[1]) : std::fabs(actual[1] - vy) <= 1e-6f, "pack motion y differs");
      expect(actual[2] == (valid ? 1.f : 0.f) && actual[3] == 0, "pack motion validity differs");
    }
    float expectedExposure = (params.exposureAvailable ? exposure : 1.f) * 3.f / 2.f;
    if (!std::isfinite(expectedExposure) || expectedExposure <= 0) expectedExposure = 1;
    expect(result[pixels * 2][0] == expectedExposure, "pack exposure normalization/fallback differs");
    context.waitIdle(); context.destroyBuffer(host);
    printf("interop pack case %u: color rectangle, motion scale/jitter/OOB/NaN, exposure %.3f PASS\n", test, expectedExposure);
  }
}
void binaryAbiCases(ID3D12Device* device, ID3D12CommandQueue* queue, const std::filesystem::path& repository) {
  struct Library {
    HMODULE module = nullptr;
    ~Library() { if (module) FreeLibrary(module); }
  } library;
  library.module = LoadLibraryW(runtimeLibraryPath(repository).c_str());
  expect(library.module != nullptr, "cannot load built OpenNrRuntime.dll");
  auto getApi = reinterpret_cast<decltype(&OpenNrGetApi)>(GetProcAddress(library.module, "OpenNrGetApi"));
  expect(getApi != nullptr, "OpenNrGetApi DLL export missing");
  OpenNrApi api{sizeof(OpenNrApi)};
  expect(getApi(OPEN_NR_ABI_VERSION + 1, &api) == LMXXF_NR_UNSUPPORTED_ABI, "DLL accepts unsupported ABI version");
  api.struct_size = sizeof(api) - 1;
  expect(getApi(OPEN_NR_ABI_VERSION, &api) != LMXXF_NR_OK, "DLL accepts truncated API structure");
  api.struct_size = sizeof(api);
  expect(getApi(OPEN_NR_ABI_VERSION, &api) == LMXXF_NR_OK && api.abi_version == OPEN_NR_ABI_VERSION,
         "DLL OpenNrApi negotiation failed");
  expect(api.QueryDeviceCapabilities && api.SetFrameMetadata && api.GetTimings && api.lifecycle.Create && api.lifecycle.Destroy &&
         api.lifecycle.GetStatus && api.lifecycle.GetLastError && api.lifecycle.ResetHistory && api.lifecycle.Drain && api.lifecycle.Poll,
         "DLL API function table incomplete");
  OpenNrDeviceCapabilities capabilities{sizeof(OpenNrDeviceCapabilities)};
  expect(api.QueryDeviceCapabilities(nullptr, &capabilities) == LMXXF_NR_INVALID_ARGUMENT, "DLL accepts null capability device");
  capabilities.struct_size = sizeof(capabilities) - 1;
  expect(api.QueryDeviceCapabilities(device, &capabilities) == LMXXF_NR_INVALID_ARGUMENT, "DLL accepts truncated capability structure");
  capabilities.struct_size = sizeof(capabilities);
  if (api.QueryDeviceCapabilities(device, &capabilities) != LMXXF_NR_OK) {
    char error[1024]{}; api.lifecycle.GetLastError(error, sizeof(error)); throw std::runtime_error(std::string("DLL device capability query: ") + error);
  }
  expect(capabilities.supported && capabilities.vendor_id == 0x1002 && capabilities.device_id == 0x7550 &&
         capabilities.fp8_e4m3 && capabilities.accumulator_fp32 && capabilities.shared_buffers && capabilities.shared_timeline_fences &&
         capabilities.matrix_m == 16 && capabilities.matrix_n == 16 && capabilities.matrix_k == 16 && capabilities.wave_size == 32 &&
         capabilities.device_name[0] && capabilities.driver[0] && capabilities.arithmetic_mode[0], "DLL actual GPU capabilities incomplete");
  printf("interop DLL query: %s; %s; %s; shared buffers/fences PASS\n", capabilities.device_name, capabilities.driver, capabilities.arithmetic_mode);
  const auto absentAssets = (repository / "build/interop/no-model-assets").wstring();
  expect(!std::filesystem::exists(absentAssets), "model-free ABI test directory unexpectedly exists");
  AssetEnvironment assetEnvironment(absentAssets);
  LmxxfNrCreateInfo createInfo{sizeof(LmxxfNrCreateInfo)};
  createInfo.device = device; createInfo.queue = queue; createInfo.assets_directory = absentAssets.c_str();
  void* session = nullptr; expect(api.lifecycle.Create(&createInfo, &session) == LMXXF_NR_OK && session, "DLL model-free session creation failed");
  try {
    OpenNrFrameMetadata metadata{sizeof(OpenNrFrameMetadata), OPEN_NR_ABI_VERSION}; metadata.pre_exposure = metadata.exposure_scale = 1;
    expect(api.SetFrameMetadata(session, &metadata) == LMXXF_NR_OK, "DLL frame metadata failed");
    metadata.abi_version = OPEN_NR_ABI_VERSION + 1;
    expect(api.SetFrameMetadata(session, &metadata) == LMXXF_NR_INVALID_ARGUMENT, "DLL accepts unsupported metadata version");
    char status[512]{}; expect(api.lifecycle.GetStatus(session, status, sizeof(status)) == LMXXF_NR_OK && status[0], "DLL session status failed");
    expect(api.lifecycle.ResetHistory(session) == LMXXF_NR_OK, "DLL model-free history reset failed");
    OpenNrTimings timings{sizeof(OpenNrTimings)};
    expect(api.GetTimings(session, &timings) == LMXXF_NR_UNAVAILABLE, "DLL fabricates timings before completed GPU work");
    timings.struct_size = sizeof(timings) - 1;
    expect(api.GetTimings(session, &timings) == LMXXF_NR_INVALID_ARGUMENT, "DLL accepts truncated timing structure");
    uint32_t state = 0;
    expect(api.lifecycle.Poll(session, reinterpret_cast<void*>(uintptr_t(0x1234)), &state) != LMXXF_NR_OK,
           "DLL accepts a job that does not belong to the session");
    for (int attempt = 0; attempt < 2; ++attempt) {
      expect(api.lifecycle.PrepareSession(session) != LMXXF_NR_OK, "DLL prepares session without shader/model assets");
      char error[1024]{}; expect(api.lifecycle.GetLastError(error, sizeof(error)) == LMXXF_NR_OK && error[0], "DLL asset failure lacks diagnostic");
    }
    expect(api.lifecycle.Drain(session) == LMXXF_NR_OK, "DLL failed-session drain failed");
    expect(api.lifecycle.Destroy(session) == LMXXF_NR_OK, "DLL failed-session destruction failed"); session = nullptr;
  } catch (...) { if (session) api.lifecycle.Destroy(session); throw; }
  printf("interop DLL ABI/lifecycle: version/size/null/foreign-job checks, metadata/reset, repeated prepare failure, drain/destroy PASS\n");
}
void syntheticRuntimeCase(ID3D12Device* device, ID3D12CommandQueue* queue, const std::filesystem::path& repository) {
  if (!getenv("OPEN_NR_RUNTIME_SELFTEST")) return;
  const char* configuredAssets = getenv("OPEN_NR_RUNTIME_ASSETS");
  const bool synthetic = !configuredAssets || !*configuredAssets;
  const char* label = synthetic ? "SYNTHETIC" : "IMPORTED";
  const auto assets = synthetic ? repository / "build/synthetic-runtime-assets/open-nr" : std::filesystem::path(configuredAssets);
  expect(std::filesystem::exists(assets / "model/manifest.json"), "runtime model assets missing");
  struct Trace {
    std::filesystem::path flag, csv; bool created = false, enabled = false; size_t priorRows = 0;
    ~Trace() { if (created) { std::error_code ignored; std::filesystem::remove(flag, ignored); } }
    std::vector<std::string> rows() const {
      std::ifstream file(csv); std::string line; std::vector<std::string> result;
      while (std::getline(file, line)) if (!line.empty()) result.push_back(line); return result;
    }
  } trace;
  struct Capture {
    std::filesystem::path flag, directory; bool created = false, enabled = false;
    uint32_t requested = 0;
    std::vector<std::filesystem::path> prior;
    ~Capture() { if (created) { std::error_code ignored; std::filesystem::remove(flag, ignored); } }
  } capture;
  if (getenv("OPEN_NR_RUNTIME_CAPTURE_SELFTEST")) {
    capture.enabled = true; capture.flag = assets / "capture.flag"; capture.directory = assets / "captures";
    if (std::filesystem::exists(capture.directory)) for (const auto& entry : std::filesystem::directory_iterator(capture.directory)) if (entry.is_directory()) capture.prior.push_back(entry.path());
    const char* requested = getenv("OPEN_NR_RUNTIME_CAPTURE_FRAMES");
    const uint32_t requestedCount = nr::capture::frameCount(requested ? std::string_view(requested) : std::string_view("1"));
    if (!std::filesystem::exists(capture.flag)) {
      std::ofstream marker(capture.flag); marker << requestedCount; expect(bool(marker), "cannot enable bounded diagnostic capture"); capture.created = true;
    }
    std::ifstream marker(capture.flag, std::ios::binary);
    expect(bool(marker), "cannot inspect existing diagnostic capture request");
    const std::string contents((std::istreambuf_iterator<char>(marker)), {});
    capture.requested = nr::capture::frameCount(contents);
    expect(!requested || capture.requested == requestedCount, "existing capture.flag differs from OPEN_NR_RUNTIME_CAPTURE_FRAMES");
  }
  if (getenv("OPEN_NR_RUNTIME_TRACE_SELFTEST")) {
    trace.enabled = true; trace.flag = assets / "record-timings.flag"; trace.csv = assets / "gpu-timings.csv";
    trace.priorRows = trace.rows().size();
    if (!std::filesystem::exists(trace.flag)) { std::ofstream marker(trace.flag); expect(bool(marker), "cannot enable runtime timing trace"); trace.created = true; }
  }
  AssetEnvironment assetEnvironment(assets.wstring());
  struct Runtime {
    HMODULE module = nullptr; OpenNrApi api{sizeof(OpenNrApi)}; void* session = nullptr;
    ~Runtime() {
      if (session && api.lifecycle.Destroy(session) != LMXXF_NR_OK) return;
      if (module) FreeLibrary(module);
    }
  } runtime;
  runtime.module = LoadLibraryW(runtimeLibraryPath(repository).c_str()); expect(runtime.module != nullptr, "synthetic runtime DLL missing");
  auto getApi = reinterpret_cast<decltype(&OpenNrGetApi)>(GetProcAddress(runtime.module, "OpenNrGetApi"));
  expect(getApi && getApi(OPEN_NR_ABI_VERSION, &runtime.api) == LMXXF_NR_OK, "synthetic runtime ABI negotiation failed");
  auto success = [&](int32_t result, const char* step) {
    if (result == LMXXF_NR_OK) return;
    char text[2048]{}; runtime.api.lifecycle.GetLastError(text, sizeof(text));
    throw std::runtime_error(std::string(step) + ": " + text + " (status " + std::to_string(result) + ")");
  };
  LmxxfNrCreateInfo createInfo{sizeof(LmxxfNrCreateInfo)}; createInfo.device = device; createInfo.queue = queue;
  success(runtime.api.lifecycle.Create(&createInfo, &runtime.session), "synthetic session creation");
  success(runtime.api.lifecycle.PrepareSession(runtime.session), "synthetic hash-verified model prepare");
  auto setting = [](const char* name, uint32_t fallback, uint32_t minimum, uint32_t maximum) {
    const char* value = getenv(name); if (!value || !*value) return fallback;
    char* end = nullptr; const unsigned long parsed = strtoul(value, &end, 10);
    expect(end && !*end && parsed >= minimum && parsed <= maximum, "invalid runtime test extent/frame setting");
    return static_cast<uint32_t>(parsed);
  };
  const uint32_t renderWidth = setting("OPEN_NR_RUNTIME_WIDTH", 320, 192, 3840);
  const uint32_t renderHeight = setting("OPEN_NR_RUNTIME_HEIGHT", 320, 128, 2160);
  const uint32_t resizeWidth = renderWidth == 192 ? 160 : 192, resizeHeight = renderHeight == 128 ? 96 : 128;
  // Capture jobs are intentionally excluded from ordinary timing publication.
  // Follow the requested sequence with one ordinary frame before timing checks.
  const uint32_t frameCount = std::max(setting("OPEN_NR_RUNTIME_FRAMES", 2, 2, 120), capture.requested + 1);
  printf("interop %s DLL actual model lifecycle/benchmark: %ux%u, %u frames, assets %s\n", label, renderWidth, renderHeight, frameCount, assets.string().c_str());
  std::vector<std::array<uint16_t, 4>> colors(renderWidth * renderHeight);
  std::vector<std::array<uint16_t, 2>> motion(renderWidth * renderHeight);
  for (uint32_t y = 0; y < renderHeight; ++y) for (uint32_t x = 0; x < renderWidth; ++x)
    colors[y * renderWidth + x] = {num::f16Bits(.25f + float(x % 8) / 32), num::f16Bits(.5f + float(y % 8) / 32), num::f16Bits(.75f), num::f16Bits(.5f)};
  D3DCommands upload; upload.create(device); upload.begin(); TestTexture colorTexture, motionTexture;
  colorTexture.create(device, upload.list.Get(), renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, colors.data());
  motionTexture.create(device, upload.list.Get(), renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT, 4, motion.data());
  upload.submit(queue); Completion completion; completion.create(device); check(queue->Signal(completion.fence.Get(), 1), "synthetic upload completion"); completion.wait(1);
  OpenNrFrameMetadata metadata{sizeof(OpenNrFrameMetadata), OPEN_NR_ABI_VERSION}; metadata.pre_exposure = metadata.exposure_scale = 1;
  success(runtime.api.SetFrameMetadata(runtime.session, &metadata), "synthetic metadata");
  auto info = [&](uint64_t frame, uint32_t width, uint32_t height) {
    LmxxfNrFrameInfo f{sizeof(LmxxfNrFrameInfo)}; f.frame_id = frame; f.color_width = width; f.color_height = height;
    f.color = colorTexture.texture.Get(); f.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f.motion = motionTexture.texture.Get(); f.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; f.motion_width = renderWidth; f.motion_height = renderHeight;
    f.motion_scale_x = f.motion_scale_y = f.transfer_strength = f.color_strength = f.model_scale = 1;
    f.flags = LMXXF_NR_FRAME_FLAG_TEMPORAL | LMXXF_NR_FRAME_FLAG_STRENGTH; f.passes = 1; return f;
  };
  auto invalidInfo = info(0, renderWidth, renderHeight); invalidInfo.flags |= 0x80000000u;
  LmxxfNrJob invalid{sizeof(LmxxfNrJob)}; invalid.handle = invalid.private_output = reinterpret_cast<void*>(uintptr_t(1));
  expect(runtime.api.lifecycle.PrepareFrame(runtime.session, &invalidInfo, &invalid) != LMXXF_NR_OK && !invalid.handle && !invalid.private_output,
         "runtime accepted unknown flags or exposed a rejected job");
  // Fail after both valid D3D12 source resources have been retained. The public
  // COM count is used only as a test diagnostic, with no count-dependent logic.
  auto referenceCount = [](ID3D12Resource* resource) { const auto count = resource->AddRef(); resource->Release(); return count; };
  const auto colorReferences = referenceCount(colorTexture.texture.Get()), motionReferences = referenceCount(motionTexture.texture.Get());
  invalidInfo = info(0, renderWidth, renderHeight); invalidInfo.motion_width = renderWidth + 1;
  invalid = {sizeof(LmxxfNrJob)};
  expect(runtime.api.lifecycle.PrepareFrame(runtime.session, &invalidInfo, &invalid) != LMXXF_NR_OK && !invalid.handle && !invalid.private_output,
         "runtime accepted an invalid motion rectangle or exposed partial preparation");
  expect(referenceCount(colorTexture.texture.Get()) == colorReferences && referenceCount(motionTexture.texture.Get()) == motionReferences,
         "failed preparation retained source resources");
  auto arrayTexture = [&](DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = renderWidth; desc.Height = renderHeight;
    desc.DepthOrArraySize = 2; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = format;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource)), "array rejection test texture");
    return resource;
  };
  auto colorArray = arrayTexture(DXGI_FORMAT_R16G16B16A16_FLOAT), motionArray = arrayTexture(DXGI_FORMAT_R16G16_FLOAT), exposureArray = arrayTexture(DXGI_FORMAT_R16_FLOAT);
  auto rejectArray = [&](LmxxfNrFrameInfo parameters, const char* failure) {
    LmxxfNrJob output{sizeof(LmxxfNrJob)}; output.handle = output.private_output = reinterpret_cast<void*>(uintptr_t(1));
    expect(runtime.api.lifecycle.PrepareFrame(runtime.session, &parameters, &output) != LMXXF_NR_OK && !output.handle && !output.private_output, failure);
    expect(referenceCount(colorTexture.texture.Get()) == colorReferences && referenceCount(motionTexture.texture.Get()) == motionReferences,
           "array rejection retained valid source resources");
    success(runtime.api.lifecycle.Drain(runtime.session), "runtime drain after array rejection");
  };
  invalidInfo = info(0, renderWidth, renderHeight); invalidInfo.color = colorArray.Get(); rejectArray(invalidInfo, "runtime accepted array color");
  invalidInfo = info(0, renderWidth, renderHeight); invalidInfo.motion = motionArray.Get(); rejectArray(invalidInfo, "runtime accepted array motion");
  auto exposureMetadata = metadata; exposureMetadata.exposure = exposureArray.Get(); exposureMetadata.exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  success(runtime.api.SetFrameMetadata(runtime.session, &exposureMetadata), "array exposure metadata");
  invalidInfo = info(0, renderWidth, renderHeight); rejectArray(invalidInfo, "runtime accepted array exposure");
  success(runtime.api.SetFrameMetadata(runtime.session, &metadata), "restore valid exposure metadata after rejection");
  success(runtime.api.lifecycle.Drain(runtime.session), "runtime drain after failed preparation");
  auto canceledInfo = info(0, renderWidth, renderHeight); LmxxfNrJob canceled{sizeof(LmxxfNrJob)};
  success(runtime.api.lifecycle.PrepareFrame(runtime.session, &canceledInfo, &canceled), "synthetic prepared job");
  success(runtime.api.lifecycle.CancelUnsubmitted(runtime.session, canceled.handle), "synthetic cancel before submission");
  struct Frame {
    D3DCommands producer, consumer; LmxxfNrJob job{sizeof(LmxxfNrJob)};
    ComPtr<ID3D12Resource> readback; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; uint32_t width = 0, height = 0;
  } frames[2];
  std::vector<std::array<uint16_t, 4>> firstPublication;
  ComPtr<ID3D12CommandQueue> foreignQueue;
  D3D12_COMMAND_QUEUE_DESC foreignQueueDesc{}; foreignQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(device->CreateCommandQueue(&foreignQueueDesc, IID_PPV_ARGS(&foreignQueue)), "foreign same-device DIRECT queue test");
  bool rejectedForeignQueue = false;
  for (auto& frame : frames) { frame.producer.create(device); frame.consumer.create(device); }
  auto recordFrame = [&](Frame& frame, uint64_t id, uint32_t width, uint32_t height) {
    auto parameters = info(id, width, height); frame.job = {sizeof(LmxxfNrJob)}; frame.width = width; frame.height = height;
    success(runtime.api.lifecycle.PrepareFrame(runtime.session, &parameters, &frame.job), "synthetic frame prepare");
    auto output = static_cast<ID3D12Resource*>(frame.job.private_output); expect(output != nullptr, "synthetic private output missing");
    auto desc = output->GetDesc(); UINT rows; UINT64 rowBytes, total;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &frame.footprint, &rows, &rowBytes, &total);
    frame.readback = testBuffer(device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    frame.producer.begin(); frame.consumer.begin();
    success(runtime.api.lifecycle.RecordInputs(runtime.session, frame.job.handle, frame.producer.list.Get()), "synthetic input pack recording");
    success(runtime.api.lifecycle.RecordOutputs(runtime.session, frame.job.handle, frame.consumer.list.Get()), "synthetic private output recording");
    transition(frame.consumer.list.Get(), output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = output; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = frame.readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = frame.footprint;
    frame.consumer.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(frame.consumer.list.Get(), output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  };
  auto submitFrame = [&](Frame& frame, uint64_t id, uint32_t width, uint32_t height) {
    recordFrame(frame, id, width, height);
    frame.producer.submit(queue);
    if (id == 1) {
      expect(runtime.api.lifecycle.EnqueueHip(runtime.session, frame.job.handle, foreignQueue.Get()) != LMXXF_NR_OK,
             "runtime accepted a foreign same-device DIRECT queue or converted queue rejection into success fallback");
      uint32_t state = 0;
      success(runtime.api.lifecycle.Poll(runtime.session, frame.job.handle, &state), "poll after foreign queue rejection");
      expect(state == LMXXF_NR_JOB_CONSUMER_COMPLETE, "foreign queue rejection changed the recorded job state");
      rejectedForeignQueue = true;
    }
    success(runtime.api.lifecycle.EnqueueHip(runtime.session, frame.job.handle, queue), "synthetic Vulkan enqueue");
    frame.consumer.submit(queue); success(runtime.api.lifecycle.Retire(runtime.session, frame.job.handle), "synthetic consumer retirement");
  };
  auto verifyFrame = [&](Frame& frame) {
    uint8_t* mapped = nullptr; check(frame.readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "synthetic private output readback");
    uint64_t samples = 0, changedRgb = 0;
    for (uint32_t y = 0; y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
      auto* pixel = reinterpret_cast<const uint16_t*>(mapped + frame.footprint.Offset + size_t(y) * frame.footprint.Footprint.RowPitch + x * 8);
      for (uint32_t channel = 0; channel < 4; ++channel) { expect(std::isfinite(num::f16ToF32(pixel[channel])), "nonfinite synthetic runtime private output"); ++samples; }
      expect(pixel[3] == num::f16Bits(.5f), "synthetic runtime changed alpha");
      for (uint32_t channel = 0; channel < 3; ++channel) changedRgb += pixel[channel] != colors[size_t(y) * renderWidth + x][channel];
    }
    frame.readback->Unmap(0, nullptr);
    expect(changedRgb != 0, "runtime private output only copied the unmodified input");
    printf("interop %s runtime private output %ux%u: %llu finite half samples, %llu changed RGB halves, alpha preserved PASS\n", label, frame.width, frame.height, (unsigned long long)samples, (unsigned long long)changedRgb);
  };
  struct Gate {
    ComPtr<ID3D12Fence> fence;
    ~Gate() { if (fence) fence->Signal(1); }
  };
  {
    Gate gate; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "synthetic in-flight gate");
    check(queue->Wait(gate.fence.Get(), 1), "synthetic GPU queue gate");
    submitFrame(frames[0], 1, renderWidth, renderHeight); submitFrame(frames[1], 2, renderWidth, renderHeight);
    std::array<LmxxfNrJob, kRuntimeFrameSlots - 2> preparedOnly;
    for (uint32_t index = 0; index < preparedOnly.size(); ++index) {
      auto parameters = info(index + 3, renderWidth, renderHeight); preparedOnly[index] = {sizeof(LmxxfNrJob)};
      success(runtime.api.lifecycle.PrepareFrame(runtime.session, &parameters, &preparedOnly[index]), "fill bounded frame pool before busy rejection");
    }
    auto blocked = info(kRuntimeFrameSlots + 1, renderWidth, renderHeight); LmxxfNrJob third{sizeof(LmxxfNrJob)};
    expect(runtime.api.lifecycle.PrepareFrame(runtime.session, &blocked, &third) != LMXXF_NR_OK, "synthetic runtime reused an in-flight slot");
    auto resized = info(3, resizeWidth, resizeHeight);
    expect(runtime.api.lifecycle.PrepareFrame(runtime.session, &resized, &third) != LMXXF_NR_OK, "synthetic runtime resized in-flight resources");
    expect(runtime.api.lifecycle.CancelUnsubmitted(runtime.session, frames[0].job.handle) != LMXXF_NR_OK, "synthetic runtime canceled submitted work");
    for (auto& prepared : preparedOnly) success(runtime.api.lifecycle.CancelUnsubmitted(runtime.session, prepared.handle), "recycle never-recorded capacity slots");
  }
  success(runtime.api.lifecycle.Drain(runtime.session), "synthetic queued frame drain"); verifyFrame(frames[0]); verifyFrame(frames[1]);
  expect(rejectedForeignQueue, "foreign queue rejection case was omitted");
  printf("interop DLL queue identity: foreign same-device DIRECT queue rejected after output recording without success fallback; same job canonical enqueue/retire PASS\n");
  if (capture.enabled) {
    firstPublication.resize(colors.size());
    uint8_t* mapped = nullptr; check(frames[0].readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "capture first D3D12 publication readback");
    for (uint32_t y = 0; y < renderHeight; ++y)
      memcpy(firstPublication.data() + size_t(y) * renderWidth, mapped + frames[0].footprint.Offset + size_t(y) * frames[0].footprint.Footprint.RowPitch, size_t(renderWidth) * 8);
    frames[0].readback->Unmap(0, nullptr);
    std::ofstream publication(repository / "build/interop/capture-publication.rgba16", std::ios::binary | std::ios::trunc);
    publication.write(reinterpret_cast<const char*>(firstPublication.data()), firstPublication.size() * sizeof(firstPublication[0]));
    expect(bool(publication), "cannot save actual D3D12 publication diagnostic");
  }
  OpenNrTimings timings{sizeof(OpenNrTimings)};
  const auto initialTimingResult = runtime.api.GetTimings(runtime.session, &timings);
  if (capture.requested >= 2) {
    expect(initialTimingResult == LMXXF_NR_UNAVAILABLE, "captured frames leaked into ordinary runtime GPU timings");
  } else {
    success(initialTimingResult, "synthetic GPU timestamp query");
    printf("interop %s DLL counters: submitted %llu bypassed %llu; bridge %.3f inference %.3f ms\n", label,
           (unsigned long long)timings.submitted_frames, (unsigned long long)timings.bypassed_frames, timings.nr_bridge_ms, timings.inference_ms);
  // Five flags/resource refusals plus busy-slot and resize are seven bypasses.
    expect(timings.submitted_frames == 2 && timings.bypassed_frames == 7 && timings.allocated_neural_bytes > 0 &&
         std::isfinite(timings.nr_bridge_ms) && timings.inference_ms > 0 && timings.pack_ms >= 0 && timings.unpack_ms >= 0,
           "synthetic runtime timings lack completed GPU work");
  }
  auto printTimings = [&] {
    printf("interop %s DLL frame %llu GPU timestamps: pack %.3f preprocess %.3f inference %.3f composite %.3f unpack %.3f bridge %.3f ms; allocated %llu bytes\n", label,
           (unsigned long long)timings.frame_id, timings.pack_ms, timings.preprocess_ms, timings.inference_ms, timings.composite_ms, timings.unpack_ms, timings.nr_bridge_ms, (unsigned long long)timings.allocated_neural_bytes);
  };
  if (initialTimingResult == LMXXF_NR_OK) printTimings();
  for (uint64_t id = 3; id <= frameCount; ++id) {
    submitFrame(frames[id % 2], id, renderWidth, renderHeight); success(runtime.api.lifecycle.Drain(runtime.session), "warmed runtime drain");
    verifyFrame(frames[id % 2]); timings.struct_size = sizeof(timings);
    const auto timingResult = runtime.api.GetTimings(runtime.session, &timings);
    if (id <= capture.requested) {
      expect(timingResult == LMXXF_NR_UNAVAILABLE, "bounded capture leaked into ordinary runtime timings");
    } else {
      success(timingResult, "warmed runtime GPU timestamp query");
      expect(timings.frame_id == id && timings.submitted_frames == id && timings.bypassed_frames == 7 &&
             timings.allocated_neural_bytes > 0 && std::isfinite(timings.nr_bridge_ms) && timings.inference_ms > 0,
             "ordinary timing after bounded capture lacks actual completed work");
      printTimings();
    }
  }
  submitFrame(frames[0], frameCount + 1, resizeWidth, resizeHeight); success(runtime.api.lifecycle.Drain(runtime.session), "synthetic resize drain"); verifyFrame(frames[0]);
  // Values are assigned during preparation. Never execute the older lists:
  // a newer Vulkan submission must make that older value inadmissible before
  // any producer signal or compatibility fallback can hide its inputs.
  recordFrame(frames[0], frameCount + 2, resizeWidth, resizeHeight);
  submitFrame(frames[1], frameCount + 3, resizeWidth, resizeHeight);
  expect(runtime.api.lifecycle.EnqueueHip(runtime.session, frames[0].job.handle, queue) != LMXXF_NR_OK,
         "runtime accepted an older prepared fence value after a newer submission or returned success fallback");
  uint32_t olderState = 0; success(runtime.api.lifecycle.Poll(runtime.session, frames[0].job.handle, &olderState), "poll rejected older submission");
  expect(olderState == LMXXF_NR_JOB_CONSUMER_COMPLETE, "rejected older submission changed its recorded job state");
  check(frames[0].producer.list->Close(), "discard older producer list"); check(frames[0].consumer.list->Close(), "discard older consumer list");
  success(runtime.api.lifecycle.CancelUnsubmitted(runtime.session, frames[0].job.handle), "cancel older never-executed lists");
  success(runtime.api.lifecycle.Drain(runtime.session), "newer submission ordering drain"); verifyFrame(frames[1]);
  printf("interop DLL submission ordering: newer slot completes; older never-executed recorded slot rejects before signal/fallback, state preserved, discard/cancel PASS\n");
  success(runtime.api.lifecycle.ResetHistory(runtime.session), "synthetic post-frame reset");
  success(runtime.api.lifecycle.Destroy(runtime.session), "synthetic runtime destruction"); runtime.session = nullptr;
  if (capture.enabled) {
    std::vector<std::filesystem::path> created;
    if (std::filesystem::exists(capture.directory)) for (const auto& entry : std::filesystem::directory_iterator(capture.directory))
      if (entry.is_directory() && std::find(capture.prior.begin(), capture.prior.end(), entry.path()) == capture.prior.end()) created.push_back(entry.path());
    expect(created.size() == 1 && created[0].filename().string().starts_with("sequence-"), "bounded capture omitted or duplicated a sequence");
    auto bytes = [](const std::filesystem::path& path) {
      std::ifstream file(path, std::ios::binary | std::ios::ate); expect(bool(file), "capture file missing"); const auto length = file.tellg();
      expect(length >= 0, "capture file size unavailable"); std::vector<uint8_t> result(size_t(length), 0); file.seekg(0); file.read(reinterpret_cast<char*>(result.data()), result.size()); expect(bool(file), "short capture read"); return result;
    };
    struct CapturedFrame { std::filesystem::path path; json::Value manifest; };
    std::vector<CapturedFrame> capturedFrames;
    for (const auto& entry : std::filesystem::directory_iterator(created[0])) if (entry.is_directory()) {
      expect(std::filesystem::exists(entry.path() / "manifest.json"), "bounded capture left an incomplete frame directory");
      const auto manifestBytes = bytes(entry.path() / "manifest.json");
      capturedFrames.push_back({entry.path(), json::parse(std::string(manifestBytes.begin(), manifestBytes.end()))});
    }
    expect(capturedFrames.size() == capture.requested, "bounded capture omitted or exceeded requested completed frames");
    std::sort(capturedFrames.begin(), capturedFrames.end(), [](const auto& a, const auto& b) {
      return a.manifest["capture_ordinal"].integer() < b.manifest["capture_ordinal"].integer();
    });
    const auto sequenceId = created[0].filename().string();
    int64_t previousSubmission = 0, previousTick = 0;
    for (size_t index = 0; index < capturedFrames.size(); ++index) {
      const auto& frame = capturedFrames[index]; const auto& metadata = frame.manifest;
      expect(metadata["format"].str() == "OpenNR-game-capture-v1" && metadata["gameCapture"].kind == json::Value::Bool &&
             !metadata["gameCapture"].boolean && metadata["performanceRepresentative"].kind == json::Value::Bool &&
             !metadata["performanceRepresentative"].boolean, "bounded harness capture claims game origin or representative performance");
      expect(metadata["sequence_id"].str() == sequenceId && metadata["capture_ordinal"].integer() == index &&
             metadata["requested_capture_count"].integer() == capture.requested && metadata["frame_id"].integer() == index + 1 &&
             metadata["width"].integer() == renderWidth && metadata["height"].integer() == renderHeight,
             "bounded capture frame identity, ordinal, count or geometry differs");
      expect(metadata["submission_id"].integer() > previousSubmission && metadata["submitted_tick_ms"].integer() >= previousTick &&
             metadata["submitted_filetime_100ns"].integer() > 0, "bounded capture submission identity/timestamp is invalid");
      expect(metadata["reset"].kind == json::Value::Bool && metadata["history_frame_ids"].kind == json::Value::Array,
             "bounded capture omitted reset/history ancestry metadata");
      if (metadata["reset"].boolean) {
        expect(metadata["history_frame_ids"].array.empty(), "reset capture retained a history ancestor");
      } else {
        expect(index > 0 && metadata["history_frame_ids"].size() == 1 && metadata["history_frame_ids"][0].integer() == index &&
               metadata["history_submission_id"].integer() == previousSubmission,
               "bounded capture history does not descend from the previous submitted frame");
      }
      expect(std::isfinite(metadata["gpu_exposure"].number) && metadata["gpu_exposure"].number > 0 &&
             metadata["jitter"].size() == 2 && metadata["motion_scale"].size() == 2 && metadata["controls"].kind == json::Value::Object,
             "bounded capture omitted exposure/motion/jitter/control metadata");
      for (const auto* name : {"source-packed.f32", "features.f32", "previous-history.f32", "controls.bin", "pack-controls.bin", "head.f32", "scene-linear-rgba.f32"})
        expect(fileHash(frame.path / name) == metadata["filesSha256"][name].str(), "bounded captured file hash mismatch");
      previousSubmission = metadata["submission_id"].integer(); previousTick = metadata["submitted_tick_ms"].integer();
    }
    const auto& firstCapture = capturedFrames[0].path; const auto& manifest = capturedFrames[0].manifest;
    expect(manifest["format"].str() == "OpenNR-game-capture-v1" && !manifest["gameCapture"].boolean && !manifest["performanceRepresentative"].boolean, "harness capture claims game origin or representative performance");
    expect(manifest["width"].integer() == renderWidth && manifest["height"].integer() == renderHeight && manifest["frame_id"].integer() == 1, "captured frame metadata differs");
    for (const auto* name : {"source-packed.f32", "features.f32", "previous-history.f32", "controls.bin", "head.f32", "scene-linear-rgba.f32"})
      expect(fileHash(firstCapture / name) == manifest["filesSha256"][name].str(), "captured file hash mismatch");
    const auto packed = bytes(firstCapture / "source-packed.f32"), oldHistory = bytes(firstCapture / "previous-history.f32"), featureBytes = bytes(firstCapture / "features.f32"), controls = bytes(firstCapture / "controls.bin"), output = bytes(firstCapture / "scene-linear-rgba.f32");
    const auto geometry = nr::Geometry::fromValid(renderWidth, renderHeight);
    expect(packed.size() == (uint64_t(renderWidth) * renderHeight * 2 + 1) * 16 && oldHistory.size() == uint64_t(renderWidth) * renderHeight * 16 && controls.size() == 72 && featureBytes.size() == uint64_t(geometry.fullWidth) * geometry.fullHeight * 64 && output.size() == uint64_t(renderWidth) * renderHeight * 16, "capture source/features/controls/output extent differs");
    expect(std::all_of(oldHistory.begin(), oldHistory.end(), [](uint8_t value) { return value == 0; }), "first captured prior history was overwritten by a subsequent frame");
    for (size_t pixel = 0; pixel < colors.size(); ++pixel) {
      float raw[4]; memcpy(raw, packed.data() + pixel * 32, 16);
      for (uint32_t channel = 0; channel < 4; ++channel) expect(num::f32Bits(raw[channel]) == num::f32Bits(num::f16ToF32(colors[pixel][channel])), "captured packed color differs from actual D3D12 source");
    }
    for (size_t token = 0; token < uint64_t(geometry.fullWidth) * geometry.fullHeight; ++token)
      expect(!memcmp(featureBytes.data() + token * 64 + 16, featureBytes.data() + token * 64 + 28, 12), "captured reset features retained prior history");
    for (size_t pixel = 0; pixel < colors.size(); ++pixel) {
      float rgba[4]; memcpy(rgba, output.data() + pixel * 16, 16);
      for (uint32_t channel = 0; channel < 4; ++channel) {
        expect(std::isfinite(rgba[channel]), "nonfinite captured composite");
        if (d3dHalfStore(rgba[channel]) != firstPublication[pixel][channel]) {
          printf("capture publication mismatch pixel %llu channel %u F32 %.9g bits %08x RTZhalf %04x actualhalf %04x\n", (unsigned long long)pixel, channel, rgba[channel], num::f32Bits(rgba[channel]), d3dHalfStore(rgba[channel]), firstPublication[pixel][channel]);
          throw std::runtime_error("captured Vulkan F32 composite does not reproduce actual D3D12 RGBA16F publication");
        }
      }
      expect(num::f16Bits(rgba[3]) == colors[pixel][3], "captured composite alpha differs");
    }
    printf("interop bounded capture: %u frames, ordered identity/ancestry/exposure metadata, all file hashes; first-frame exact actual source, untouched prior history, reset features and exact F32-to-actual-D3D12-RGBA16F RTZ publication; truthful synthetic origin PASS (%s; excluded from performance results)\n", capture.requested, created[0].string().c_str());
  }
  if (trace.enabled) {
    auto rows = trace.rows(); const size_t priorData = trace.priorRows ? trace.priorRows - 1 : 0;
    expect(!rows.empty() && rows[0] == "frame_id,pack_ms,preprocess_ms,inference_ms,composite_ms,unpack_ms,neural_ms,vram_mib,allocated_neural_bytes,submitted_frames,bypassed_frames",
           "runtime timing trace header mismatch");
    expect(rows.size() == priorData + frameCount + 3 - capture.requested, "runtime timing trace omitted/duplicated ordinary jobs or included diagnostic captures");
    for (size_t index = priorData + 1; index < rows.size(); ++index) {
      std::stringstream line(rows[index]); std::string value; std::vector<double> fields;
      while (std::getline(line, value, ',')) fields.push_back(std::stod(value));
      expect(fields.size() == 11 && fields[3] > 0 && fields[7] > 0 && fields[8] > 0, "runtime trace lacks GPU timing/actual process VRAM/allocation samples");
      for (auto field : fields) expect(std::isfinite(field), "runtime timing trace contains nonfinite metadata");
    }
    printf("interop DLL trace: %u ordinary completed frames, %u diagnostic frames excluded, positive DXGI process local VRAM, finite GPU timestamps, no duplicate reclaim rows PASS (%s)\n", frameCount + 2 - capture.requested, capture.requested, trace.csv.string().c_str());
  }
  printf("interop %s DLL lifecycle: verified model, flags/array rejection/failed-prepare cleanup, cancel, two queued slots, in-flight refusal, timestamps, drain/resize/reset/destroy PASS\n", label);
}
void recoveryRuntimeCases(ID3D12Device* device, ID3D12CommandQueue* canonicalQueue, const std::filesystem::path& repository) {
  if (!getenv("OPEN_NR_RUNTIME_SELFTEST")) return;
  const char* configuredAssets = getenv("OPEN_NR_RUNTIME_ASSETS");
  const auto assets = configuredAssets && *configuredAssets ? std::filesystem::path(configuredAssets) : repository / "build/synthetic-runtime-assets/open-nr";
  AssetEnvironment assetEnvironment(assets.wstring());
  constexpr uint32_t width = 320, height = 320;
  std::vector<std::array<uint16_t, 4>> colors(width * height);
  std::vector<std::array<uint16_t, 2>> motion(width * height);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
    colors[y * width + x] = {num::f16Bits(.25f + float(x % 8) / 32), num::f16Bits(.5f + float(y % 8) / 32), num::f16Bits(.75f), num::f16Bits(.5f)};
  D3DCommands upload; upload.create(device); upload.begin(); TestTexture colorTexture, motionTexture;
  colorTexture.create(device, upload.list.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, colors.data());
  motionTexture.create(device, upload.list.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, 4, motion.data());
  upload.submit(canonicalQueue); Completion uploadDone; uploadDone.create(device);
  check(canonicalQueue->Signal(uploadDone.fence.Get(), 1), "recovery source upload completion"); uploadDone.wait(1);
  ComPtr<ID3D12CommandQueue> foreignQueue; D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&foreignQueue)), "recovery foreign producer queue");
  for (uint32_t mode = 0; mode < 3; ++mode) {
    struct Runtime {
      HMODULE module = nullptr; OpenNrApi api{sizeof(OpenNrApi)}; void* session = nullptr;
      ~Runtime() { if (session && api.lifecycle.Destroy(session) != LMXXF_NR_OK) return; if (module) FreeLibrary(module); }
    } runtime;
    runtime.module = LoadLibraryW(runtimeLibraryPath(repository).c_str()); expect(runtime.module != nullptr, "recovery runtime DLL unavailable");
    auto getApi = reinterpret_cast<decltype(&OpenNrGetApi)>(GetProcAddress(runtime.module, "OpenNrGetApi"));
    auto recover = reinterpret_cast<decltype(&OpenNrRecoverSubmission)>(GetProcAddress(runtime.module, "OpenNrRecoverSubmission"));
    expect(getApi && recover && getApi(OPEN_NR_ABI_VERSION, &runtime.api) == LMXXF_NR_OK, "recovery export/ABI unavailable");
    auto success = [&](int32_t result, const char* step) {
      if (result == LMXXF_NR_OK) return;
      char error[2048]{}; runtime.api.lifecycle.GetLastError(error, sizeof(error));
      throw std::runtime_error(std::string(step) + ": " + error + " (status " + std::to_string(result) + ")");
    };
    LmxxfNrCreateInfo creation{sizeof(LmxxfNrCreateInfo)}; creation.device = device; creation.queue = canonicalQueue;
    success(runtime.api.lifecycle.Create(&creation, &runtime.session), "recovery session create");
    success(runtime.api.lifecycle.PrepareSession(runtime.session), "recovery hash-verified model prepare");
    OpenNrFrameMetadata metadata{sizeof(OpenNrFrameMetadata), OPEN_NR_ABI_VERSION}; metadata.pre_exposure = metadata.exposure_scale = 1;
    success(runtime.api.SetFrameMetadata(runtime.session, &metadata), "recovery metadata");
    LmxxfNrFrameInfo parameters{sizeof(LmxxfNrFrameInfo)}; parameters.frame_id = 1; parameters.color_width = parameters.motion_width = width;
    parameters.color_height = parameters.motion_height = height; parameters.color = colorTexture.texture.Get(); parameters.motion = motionTexture.texture.Get();
    parameters.color_state = parameters.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    parameters.motion_scale_x = parameters.motion_scale_y = parameters.transfer_strength = parameters.color_strength = parameters.model_scale = 1;
    parameters.flags = LMXXF_NR_FRAME_FLAG_TEMPORAL | LMXXF_NR_FRAME_FLAG_STRENGTH; parameters.passes = 1;
    LmxxfNrJob job{sizeof(LmxxfNrJob)}; success(runtime.api.lifecycle.PrepareFrame(runtime.session, &parameters, &job), "recovery frame prepare");
    auto output = static_cast<ID3D12Resource*>(job.private_output); expect(output != nullptr, "recovery private output unavailable");
    const auto desc = output->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows; UINT64 rowBytes, total;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    auto readback = testBuffer(device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3DCommands producer, consumer; producer.create(device); consumer.create(device); producer.begin(); consumer.begin();
    success(runtime.api.lifecycle.RecordInputs(runtime.session, job.handle, producer.list.Get()), "recovery input recording");
    if (mode != 2) success(runtime.api.lifecycle.RecordOutputs(runtime.session, job.handle, consumer.list.Get()), "recovery output recording");
    // Partial recording must still permit the ordinary game's continuation,
    // even when no NR unpack commands exist. Its original source stays valid.
    auto copySource = mode == 2 ? colorTexture.texture.Get() : output;
    transition(consumer.list.Get(), copySource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = copySource; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
    consumer.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(consumer.list.Get(), copySource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto actualQueue = mode ? canonicalQueue : foreignQueue.Get(); producer.submit(actualQueue);
    if (mode != 2) {
      const auto enqueueResult = runtime.api.lifecycle.EnqueueHip(runtime.session, job.handle, actualQueue);
      if (mode) success(enqueueResult, "recovery successful Vulkan enqueue");
      else expect(enqueueResult != LMXXF_NR_OK, "strict enqueue accepted the foreign producer queue");
    }
    success(recover(runtime.session, job.handle, actualQueue), "failure-only original-color recovery");
    expect(recover(runtime.session, job.handle, actualQueue) != LMXXF_NR_OK, "runtime accepted duplicate recovery before consumer retirement");
    consumer.submit(actualQueue); success(runtime.api.lifecycle.Retire(runtime.session, job.handle), "recovery consumer retire");
    success(runtime.api.lifecycle.Drain(runtime.session), "recovery completion drain");
    uint8_t* mapped = nullptr; check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "recovered private output readback");
    for (uint32_t y = 0; y < height; ++y)
      expect(!memcmp(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, colors.data() + size_t(y) * width, size_t(width) * 8), "recovery did not publish exact original RGBA16F color/alpha");
    readback->Unmap(0, nullptr);
    parameters.frame_id = 2; LmxxfNrJob rejected{sizeof(LmxxfNrJob)}; rejected.handle = rejected.private_output = reinterpret_cast<void*>(uintptr_t(1));
    expect(runtime.api.lifecycle.PrepareFrame(runtime.session, &parameters, &rejected) != LMXXF_NR_OK && !rejected.handle && !rejected.private_output,
           "recovered failed session accepted another frame or exposed rejected resources");
    success(runtime.api.lifecycle.Destroy(runtime.session), "recovery session destroy"); runtime.session = nullptr;
    printf("interop DLL recovery %s: exact 409600 original RGBA16F channels, duplicate recovery rejection, failed-session refusal, retire/drain/destroy PASS\n",
           mode == 2 ? "partial recording without NR outputs, ordinary source continuation" : mode == 1 ? "after successful Vulkan enqueue" : "foreign actual producer queue");
  }
}
void queuedRuntimePoolCases(ID3D12Device* device, ID3D12CommandQueue* queue, const std::filesystem::path& repository) {
  if (!getenv("OPEN_NR_RUNTIME_SELFTEST")) return;
  const char* configuredAssets = getenv("OPEN_NR_RUNTIME_ASSETS");
  const auto assets = configuredAssets && *configuredAssets ? std::filesystem::path(configuredAssets) : repository / "build/synthetic-runtime-assets/open-nr";
  AssetEnvironment environment(assets.wstring());
  // Use the same requested geometry as the DLL lifecycle case above. With no
  // extent selection this remains the historical 320x320 pool test.
  auto setting = [](const char* name, uint32_t fallback, uint32_t minimum, uint32_t maximum) {
    const char* value = getenv(name); if (!value || !*value) return fallback;
    char* end = nullptr; const unsigned long parsed = strtoul(value, &end, 10);
    expect(end && !*end && parsed >= minimum && parsed <= maximum, "invalid pool test extent setting");
    return static_cast<uint32_t>(parsed);
  };
  const uint32_t width = setting("OPEN_NR_RUNTIME_WIDTH", 320, 192, 3840);
  const uint32_t height = setting("OPEN_NR_RUNTIME_HEIGHT", 320, 128, 2160);
  const uint32_t resizeWidth = width == 192 ? 160 : 192, resizeHeight = height == 128 ? 96 : 128;
  constexpr uint32_t count = kRuntimeFrameSlots;
  static_assert(count == 8, "the bounded pool qualification requires eight live slots");
  const auto geometry = nr::Geometry::fromValid(width, height);
  printf("interop source-included production eight-slot pool: requested %ux%u, padded %ux%u, %u slots; synthetic diagnostic readback, excluded from performance results\n",
         width, height, geometry.fullWidth, geometry.fullHeight, count);
  std::vector<std::array<uint16_t, 4>> colors(width * height);
  std::vector<std::array<uint16_t, 2>> motion(width * height);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
    colors[y * width + x] = {num::f16Bits(.25f + float(x % 8) / 32), num::f16Bits(.5f + float(y % 8) / 32), num::f16Bits(.75f), num::f16Bits(.5f)};
  D3DCommands upload; upload.create(device); upload.begin(); TestTexture color, velocity;
  color.create(device, upload.list.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, colors.data());
  velocity.create(device, upload.list.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, 4, motion.data());
  upload.submit(queue); Completion uploaded; uploaded.create(device); check(queue->Signal(uploaded.fence.Get(), 1), "pool source upload completion"); uploaded.wait(1);
  // This source-included production runtime lets the test inspect final
  // enqueue-time controls without extending the public DLL ABI. The DLL's
  // actual eight-slot capacity and recovery paths are tested above separately.
  OpenNrApi api{sizeof(OpenNrApi)}; expect(OpenNrGetApi(OPEN_NR_ABI_VERSION, &api) == LMXXF_NR_OK, "pool production ABI");
  auto success = [&](int32_t result, const char* what) {
    if (result == LMXXF_NR_OK) return; char error[2048]{}; api.lifecycle.GetLastError(error, sizeof(error));
    throw std::runtime_error(std::string(what) + ": " + error);
  };
  using Outputs = std::array<std::vector<uint8_t>, count>;
  auto run = [&](bool prefetched, bool edges) {
    struct Owner { OpenNrApi& api; void* pointer = nullptr; ~Owner(){ if(pointer)api.lifecycle.Destroy(pointer); } } session{api};
    LmxxfNrCreateInfo creation{sizeof(LmxxfNrCreateInfo)}; creation.device = device; creation.queue = queue;
    success(api.lifecycle.Create(&creation, &session.pointer), "pool create"); success(api.lifecycle.PrepareSession(session.pointer), "pool model prepare");
    auto& sourceSession = *static_cast<Session*>(session.pointer);
    struct Frame { LmxxfNrJob job{sizeof(LmxxfNrJob)}; D3DCommands producer, consumer; ComPtr<ID3D12Resource> readback; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; } frames[count];
    auto prepare = [&](uint32_t index) {
      OpenNrFrameMetadata metadata{sizeof(OpenNrFrameMetadata), OPEN_NR_ABI_VERSION}; metadata.pre_exposure = metadata.exposure_scale = 1;
      metadata.jitter_x = float(index) / 8; metadata.jitter_y = float(int(index % 3) - 1) / 16;
      success(api.SetFrameMetadata(session.pointer, &metadata), "pool jitter metadata");
      LmxxfNrFrameInfo parameters{sizeof(LmxxfNrFrameInfo)}; parameters.frame_id = index + 1 + (edges && index >= 5 ? 1 : 0);
      parameters.color_width = parameters.motion_width = width; parameters.color_height = parameters.motion_height = height;
      parameters.color = color.texture.Get(); parameters.motion = velocity.texture.Get(); parameters.color_state = parameters.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      parameters.motion_scale_x = parameters.motion_scale_y = parameters.transfer_strength = parameters.color_strength = parameters.model_scale = 1;
      parameters.flags = LMXXF_NR_FRAME_FLAG_TEMPORAL | LMXXF_NR_FRAME_FLAG_STRENGTH; parameters.passes = 1;
      auto& frame = frames[index]; success(api.lifecycle.PrepareFrame(session.pointer, &parameters, &frame.job), "pool frame prepare");
      auto& job = *static_cast<Job*>(frame.job.handle);
      expect(job.params.width == width && job.params.height == height &&
             job.params.fullWidth == geometry.fullWidth && job.params.fullHeight == geometry.fullHeight,
             "prepared pool job does not use requested runtime geometry");
      expect(job.params.historyValid == 0 && job.params.seed == 0, "preparation prematurely publishes temporal controls");
      if (job.packedConsecutive) {
        expect(index && job.pack.jitterDeltaX == -1.0f / 8 && job.pack.jitterDeltaY == float(int((index - 1) % 3) - int(index % 3)) / 16,
               "packed jitter does not match the consecutive prepared predecessor");
      } else expect(job.pack.jitterDeltaX == 0 && job.pack.jitterDeltaY == 0, "discontinuous prepared jitter was retained");
      frame.producer.create(device); frame.consumer.create(device); frame.producer.begin(); frame.consumer.begin();
      success(api.lifecycle.RecordInputs(session.pointer, frame.job.handle, frame.producer.list.Get()), "pool record inputs");
      success(api.lifecycle.RecordOutputs(session.pointer, frame.job.handle, frame.consumer.list.Get()), "pool record outputs");
      auto output = static_cast<ID3D12Resource*>(frame.job.private_output); const auto desc = output->GetDesc(); UINT rows; UINT64 rowBytes, total;
      expect(desc.Width == width && desc.Height == height, "pool output texture does not use requested runtime geometry");
      device->GetCopyableFootprints(&desc, 0, 1, 0, &frame.footprint, &rows, &rowBytes, &total);
      frame.readback = testBuffer(device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      transition(frame.consumer.list.Get(), output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = output; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = frame.readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = frame.footprint;
      frame.consumer.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(frame.consumer.list.Get(), output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };
    if (prefetched) {
      for (uint32_t index = 0; index < count; ++index) prepare(index);
      // This assertion happens before any queue submission, cancellation or
      // retirement: every prepared job owns a distinct live runtime slot.
      for (uint32_t index = 0; index < count; ++index) {
        const auto& job = sourceSession.jobs[index];
        expect(job && frames[index].job.handle == job.get() &&
               job->state == LMXXF_NR_JOB_CONSUMER_COMPLETE && !job->vulkanSubmitted && !job->retired,
               "prefetched pool did not retain eight distinct live prepared slots");
      }
      printf("interop source-included pool %s %ux%u: eight distinct live prepared slots verified before submission\n",
             edges ? "cancel/gap/reset" : "ordered continuous", width, height);
    }
    struct Gate { ComPtr<ID3D12Fence> fence; ~Gate(){ if(fence)fence->Signal(1); } } gate;
    if (prefetched) { check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "eight-slot queue gate"); check(queue->Wait(gate.fence.Get(), 1), "gate eight live frame submissions"); }
    for (uint32_t index = 0; index < count; ++index) {
      if (!prefetched) prepare(index);
      auto& frame = frames[index];
      if (edges && index == 2) {
        check(frame.producer.list->Close(), "discard canceled predecessor producer"); check(frame.consumer.list->Close(), "discard canceled predecessor consumer");
        success(api.lifecycle.CancelUnsubmitted(session.pointer, frame.job.handle), "pool canceled predecessor"); continue;
      }
      if (edges && index == 4) success(api.lifecycle.ResetHistory(session.pointer), "reset after preparation before enqueue");
      frame.producer.submit(queue); success(api.lifecycle.EnqueueHip(session.pointer, frame.job.handle, queue), "pool ordered enqueue");
      const auto& job = *static_cast<Job*>(frame.job.handle);
      const bool expectedValid = edges ? (index == 1 || index >= 6) : index != 0;
      const uint32_t expectedSeed = edges ? (index == 1 ? 1u : index >= 6 ? index - 5 : 0u) : index;
      expect(job.params.historyValid == uint32_t(expectedValid) && job.params.seed == expectedSeed, "final temporal controls ignore actual predecessor/cancel/gap/reset order");
      frame.consumer.submit(queue); success(api.lifecycle.Retire(session.pointer, frame.job.handle), "pool ordered retire");
      if (!prefetched) success(api.lifecycle.Drain(session.pointer), "serial frame drain");
    }
    if (gate.fence) check(gate.fence->Signal(1), "release eight-slot queue gate");
    success(api.lifecycle.Drain(session.pointer), "eight-slot completion drain");
    Outputs result;
    for (uint32_t index = 0; index < count; ++index) if (!edges || index != 2) {
      auto& frame = frames[index]; result[index].resize(size_t(width) * height * 8);
      uint8_t* mapped = nullptr; check(frame.readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "pool private output readback");
      for (uint32_t y = 0; y < height; ++y) memcpy(result[index].data() + size_t(y) * width * 8, mapped + frame.footprint.Offset + size_t(y) * frame.footprint.Footprint.RowPitch, size_t(width) * 8);
      frame.readback->Unmap(0, nullptr);
    }
    // A reset from a declined real game frame must invalidate preparation
    // ancestry even if an older queued frame consumes the global reset first.
    if (!edges) {
      prepare(0); success(api.lifecycle.ResetHistory(session.pointer), "prefetched reset ancestry invalidation");
      frames[0].producer.submit(queue); success(api.lifecycle.EnqueueHip(session.pointer, frames[0].job.handle, queue), "older frame consumes reset");
      frames[0].consumer.submit(queue); success(api.lifecycle.Retire(session.pointer, frames[0].job.handle), "reset ancestry retire");
      success(api.lifecycle.Drain(session.pointer), "reset ancestry drain");
      prepare(1);
      expect(static_cast<Job*>(frames[1].job.handle)->packedPredecessor == 0, "older enqueue restored ancestry across a declined game frame");
      frames[1].producer.submit(queue); success(api.lifecycle.EnqueueHip(session.pointer, frames[1].job.handle, queue), "post-decline contiguous ID enqueue");
      expect(static_cast<Job*>(frames[1].job.handle)->params.historyValid == 0 && static_cast<Job*>(frames[1].job.handle)->params.seed == 0, "post-decline contiguous prepare ID incorrectly consumes history");
      frames[1].consumer.submit(queue); success(api.lifecycle.Retire(session.pointer, frames[1].job.handle), "post-decline retire"); success(api.lifecycle.Drain(session.pointer), "post-decline drain");
    }
    LmxxfNrFrameInfo resized{sizeof(LmxxfNrFrameInfo)}; resized.frame_id = sourceSession.lastPreparedFrame + 1;
    resized.color_width = resizeWidth; resized.color_height = resizeHeight; resized.motion_width = width; resized.motion_height = height;
    resized.color = color.texture.Get(); resized.motion = velocity.texture.Get(); resized.color_state = resized.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    resized.motion_scale_x = resized.motion_scale_y = resized.transfer_strength = resized.color_strength = resized.model_scale = 1;
    resized.flags = LMXXF_NR_FRAME_FLAG_TEMPORAL | LMXXF_NR_FRAME_FLAG_STRENGTH; resized.passes = 1;
    LmxxfNrJob afterResize{sizeof(LmxxfNrJob)}; success(api.lifecycle.PrepareFrame(session.pointer, &resized, &afterResize), "pool resize ancestry preparation");
    expect(static_cast<Job*>(afterResize.handle)->packedPredecessor == 0 && !static_cast<Job*>(afterResize.handle)->packedConsecutive,
           "resize retained an old geometry's prepared predecessor");
    success(api.lifecycle.CancelUnsubmitted(session.pointer, afterResize.handle), "pool resize prepared cancellation");
    expect(sourceSession.jobs.size() == 8, "runtime frame pool is not bounded at eight slots");
    success(api.lifecycle.Destroy(session.pointer), "pool destroy"); session.pointer = nullptr; return result;
  };
  for (bool edges : {false, true}) {
    const auto serialized = run(false, edges), prefetched = run(true, edges);
    expect(serialized == prefetched, "multiple prepared frames changed temporal results versus serialized preparation/enqueue");
    const uint32_t comparedFrames = edges ? count - 1 : count;
    printf("interop source-included production eight-slot pool %s %ux%u (padded %ux%u): immutable prepared jitter, enqueue-time seed/history, eight independent descriptor lifetimes, serialized/prefetched exact output equality %u frames/%llu bytes PASS\n",
           edges ? "cancel/gap/reset" : "ordered continuous", width, height, geometry.fullWidth, geometry.fullHeight,
           comparedFrames, (unsigned long long)(uint64_t(comparedFrames) * width * height * 8));
  }
}
#if defined(NR_HOST_BINDINGS_SELFTEST)
void continuationBindingCase(ID3D12Device* device, ID3D12CommandQueue* queue, const std::filesystem::path& repository) {
  using DlssNr::Submission::CommandListProxy;
  using DlssNr::Submission::ILogicalCommandList;
  PackPipelines runtime; runtime.create(device, repository / "game/shaders/bridge.hlsl");
  const char* shader = "cbuffer P:register(b0){uint4 value;} RWByteAddressBuffer outValue:register(u0);"
                       "[numthreads(1,1,1)] void main(){outValue.Store4(0,value);}";
  ComPtr<ID3DBlob> code, errors;
  check(D3DCompile(shader, strlen(shader), "game binding sentinel", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code, &errors), "game sentinel HLSL");
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameters[2]{}; parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 4}; parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &range};
  D3D12_ROOT_SIGNATURE_DESC description{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> rootCode; check(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &rootCode, &errors), "game sentinel root serialization");
  ComPtr<ID3D12RootSignature> root; check(device->CreateRootSignature(0, rootCode->GetBufferPointer(), rootCode->GetBufferSize(), IID_PPV_ARGS(&root)), "game sentinel root");
  D3D12_COMPUTE_PIPELINE_STATE_DESC ps{}; ps.pRootSignature = root.Get(); ps.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline; check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)), "game sentinel pipeline");
  auto result = testBuffer(device, 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = testBuffer(device, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto packed = testBuffer(device, 208, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto neural = testBuffer(device, 112, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zeros = testBuffer(device, 112, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void* mapped = nullptr; check(zeros->Map(0, nullptr, &mapped), "binding test zero map"); memset(mapped, 0, 112); zeros->Unmap(0, nullptr);
  D3DCommands producer; producer.create(device); producer.begin();
  producer.list->CopyBufferRegion(neural.Get(), 0, zeros.Get(), 0, 112);
  transition(producer.list.Get(), neural.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  std::array<std::array<float, 4>, 42> color{};
  for (uint32_t y = 0; y < 6; ++y) for (uint32_t x = 0; x < 7; ++x) color[y * 7 + x] = {float(x) / 4, float(y) / 2, float(x + y) / 8, 1};
  TestTexture colorTexture; colorTexture.create(device, producer.list.Get(), 7, 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, color.data());
  D3D12_HEAP_PROPERTIES textureHeap{}; textureHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC textureDesc{}; textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; textureDesc.Width = 3; textureDesc.Height = 2;
  textureDesc.DepthOrArraySize = textureDesc.MipLevels = 1; textureDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; textureDesc.SampleDesc.Count = 1; textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> privateOutput; check(device->CreateCommittedResource(&textureHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&privateOutput)), "binding test runtime output");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows; UINT64 rowBytes, totalBytes;
  device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
  auto privateReadback = testBuffer(device, totalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> heaps[3];
  for (uint32_t index = 0; index < 3; ++index) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = index ? 4 : 1; desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heaps[index])), "binding test descriptor heap");
  }
  const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto handle = [&](UINT heap, UINT index) { auto h = heaps[heap]->GetCPUDescriptorHandleForHeapStart(); h.ptr += size_t(index) * step; return h; };
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = 4; uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW; device->CreateUnorderedAccessView(result.Get(), nullptr, &uav, handle(0, 0));
  D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv{}; textureSrv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; textureSrv.Texture2D.MipLevels = 1;
  for (UINT index = 0; index < 3; ++index) device->CreateShaderResourceView(colorTexture.texture.Get(), &textureSrv, handle(1, index));
  for (UINT index = 1; index < 3; ++index) device->CreateShaderResourceView(colorTexture.texture.Get(), &textureSrv, handle(2, index));
  D3D12_SHADER_RESOURCE_VIEW_DESC bufferSrv{}; bufferSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  bufferSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; bufferSrv.Buffer.NumElements = 7; bufferSrv.Buffer.StructureByteStride = 16;
  device->CreateShaderResourceView(neural.Get(), &bufferSrv, handle(2, 0));
  uav = {}; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Buffer.NumElements = 13; uav.Buffer.StructureByteStride = 16;
  device->CreateUnorderedAccessView(packed.Get(), nullptr, &uav, handle(1, 3));
  uav = {}; uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(privateOutput.Get(), nullptr, &uav, handle(2, 3));
  CommandListProxy* rawProxy = nullptr; check(CommandListProxy::Create(device, producer.allocator.Get(), producer.list.Get(), &rawProxy), "binding test proxy");
  ComPtr<CommandListProxy> proxy; proxy.Attach(rawProxy); ComPtr<ILogicalCommandList> logical;
  check(proxy->QueryInterface(IID_PPV_ARGS(&logical)), "binding test logical interface");
  const uint32_t values[4] = {0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000};
  const UINT clear[4]{}; ID3D12DescriptorHeap* clearHeap = heaps[0].Get();
  producer.list->SetDescriptorHeaps(1, &clearHeap);
  producer.list->ClearUnorderedAccessViewUint(heaps[0]->GetGPUDescriptorHandleForHeapStart(), handle(0, 0), result.Get(), clear, 0, nullptr);
  D3D12_RESOURCE_BARRIER clearBarrier{}; clearBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; clearBarrier.UAV.pResource = result.Get();
  producer.list->ResourceBarrier(1, &clearBarrier);
  ID3D12DescriptorHeap* gameHeap = heaps[0].Get(); proxy->SetDescriptorHeaps(1, &gameHeap);
  proxy->SetComputeRootSignature(root.Get()); proxy->SetPipelineState(pipeline.Get()); proxy->SetComputeRoot32BitConstants(0, 4, values, 0);
  proxy->SetComputeRootDescriptorTable(1, heaps[0]->GetGPUDescriptorHandleForHeapStart());
  PackParams params{3, 2, 1, 2, 3, 2, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
  auto* runtimeSegment = logical->RuntimeSegment(); ID3D12DescriptorHeap* runtimeHeap = heaps[1].Get();
  transition(runtimeSegment, packed.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  runtimeSegment->SetDescriptorHeaps(1, &runtimeHeap); runtimeSegment->SetComputeRootSignature(runtime.root.Get()); runtimeSegment->SetPipelineState(runtime.pack.Get());
  runtimeSegment->SetComputeRoot32BitConstants(0, 16, &params, 0); runtimeSegment->SetComputeRootDescriptorTable(1, heaps[1]->GetGPUDescriptorHandleForHeapStart()); runtimeSegment->Dispatch(1, 1, 1);
  transition(runtimeSegment, packed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  logical->RestoreRuntimeBindings(); check(logical->SplitSegments(), "binding test split");
  runtimeSegment = logical->RuntimeSegment(); runtimeHeap = heaps[2].Get();
  transition(runtimeSegment, privateOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  runtimeSegment->SetDescriptorHeaps(1, &runtimeHeap); runtimeSegment->SetComputeRootSignature(runtime.root.Get()); runtimeSegment->SetPipelineState(runtime.unpack.Get());
  runtimeSegment->SetComputeRoot32BitConstants(0, 16, &params, 0); runtimeSegment->SetComputeRootDescriptorTable(1, heaps[2]->GetGPUDescriptorHandleForHeapStart()); runtimeSegment->Dispatch(1, 1, 1);
  transition(runtimeSegment, privateOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = privateReadback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
  D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = privateOutput.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  runtimeSegment->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  logical->RestoreRuntimeBindings(); proxy->Dispatch(1, 1, 1);
  transition(proxy.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  proxy->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, 16); check(logical->ExecuteOn(queue), "binding test split execution");
  Completion completion; completion.create(device); check(queue->Signal(completion.fence.Get(), 1), "binding test completion"); completion.wait(1);
  D3D12_RANGE read{0, 16}; check(readback->Map(0, &read, &mapped), "binding test sentinel map");
  expect(!memcmp(mapped, values, 16), "runtime bindings overwrote game PSO/root constants/descriptor heap across cut"); readback->Unmap(0, nullptr);
  check(privateReadback->Map(0, nullptr, &mapped), "binding test fallback map");
  for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 3; ++x) {
    auto* pixel = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch + x * 8);
    for (uint32_t channel = 0; channel < 4; ++channel) expect(pixel[channel] == num::f16Bits(color[(y + 2) * 7 + x + 1][channel]), "runtime unpack marker fallback differs");
  }
  privateReadback->Unmap(0, nullptr);
  printf("interop host continuation: real pack/restore/split/unpack/restore, game PSO/root/constants/heap sentinel and private fallback PASS\n");
}
#endif
} // namespace

int main(int argc, char** argv) {
  try {
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> candidate;
      const HRESULT status = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
      if (status == DXGI_ERROR_NOT_FOUND) break;
      check(status, "enumerate high-performance D3D12 adapters");
      DXGI_ADAPTER_DESC1 desc{}; check(candidate->GetDesc1(&desc), "adapter description");
      if (desc.VendorId == 0x1002 && desc.DeviceId == 0x7550 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = candidate; break; }
    }
    expect(adapter != nullptr, "RX 9070 XT D3D12 adapter unavailable");
    ComPtr<ID3D12Device> d3d; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d)), "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; check(d3d->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "D3D12 queue");
    const auto repository = argc >= 2 ? std::filesystem::path(argv[1]) : std::filesystem::path(argv[0]).parent_path().parent_path().parent_path();
    binaryAbiCases(d3d.Get(), queue.Get(), repository);
    syntheticRuntimeCase(d3d.Get(), queue.Get(), repository);
    recoveryRuntimeCases(d3d.Get(), queue.Get(), repository);
    queuedRuntimePoolCases(d3d.Get(), queue.Get(), repository);
    LUID luid = d3d->GetAdapterLuid(); vk::Context context(vk::Backend::AmdFast, reinterpret_cast<uint8_t*>(&luid), true);
    SharedFence ready, finished; ready.create(d3d.Get(), context); finished.create(d3d.Get(), context);
    Completion completion; completion.create(d3d.Get()); uint64_t value = 0;
    transferFrames(d3d.Get(), queue.Get(), context, ready, finished, completion, 4099, 10, value);
    // Drain the prior slots, destroy imported resources, and use larger allocations
    // while keeping both imported fence timelines monotonic.
    transferFrames(d3d.Get(), queue.Get(), context, ready, finished, completion, (17u << 20) / 4, 4, value);
    packCases(d3d.Get(), queue.Get(), context, ready, finished, completion, repository, value);
#if defined(NR_HOST_BINDINGS_SELFTEST)
    continuationBindingCase(d3d.Get(), queue.Get(), repository);
#endif
    printf("INTEROP SELFTEST PASS (%s; shared D3D12 DEFAULT buffers, GPU timeline waits, drain/resize)\n", context.deviceName().c_str());
    return 0;
  } catch (const std::exception& error) { fprintf(stderr, "INTEROP SELFTEST FAIL: %s\n", error.what()); return 1; }
}
