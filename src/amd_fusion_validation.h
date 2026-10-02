// CPU-side safety contracts for the independently selected AMD fusion kernels.
// These validate operands before descriptor binding or uint-indexed dispatch.
#pragma once
#include "kernels.h"

#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

namespace nr {
namespace amd_fusion_detail {

inline void require(bool condition, const char* operation, const char* operand,
                    const char* reason) {
  if (!condition)
    throw std::runtime_error(std::string(operation) + ": " + operand + " " + reason);
}

inline uint64_t multiply(uint64_t a, uint64_t b, const char* operation,
                         const char* operand) {
  require(b == 0 || a <= std::numeric_limits<uint64_t>::max() / b,
          operation, operand, "byte count overflows");
  return a * b;
}

inline void buffer(const vk::Buffer* value, uint64_t bytes, const char* operation,
                   const char* operand) {
  require(value != nullptr, operation, operand, "is missing");
  require(value->buffer != VK_NULL_HANDLE, operation, operand, "has no Vulkan buffer");
  require(value->size >= bytes, operation, operand, "buffer is too small");
}

inline void activation(const Activation* value, Format format, uint32_t rows,
                       uint32_t channels, const char* operation, const char* operand) {
  require(value != nullptr, operation, operand, "is missing");
  require(value->format == format, operation, operand, "has the wrong format");
  // The shaders share one output stride and fixed channel tiles. Wider backing
  // strides are deliberately unsupported rather than silently truncating them.
  require(value->channels == channels, operation, operand, "has the wrong channel stride");
  require(rows != 0 && value->rows >= rows, operation, operand, "has insufficient logical rows");
  require(value->allocRows >= value->rows, operation, operand, "allocation rows are inconsistent");
  require(multiply(rows, channels, operation, operand) <= std::numeric_limits<uint32_t>::max(),
          operation, operand, "exceeds shader uint indexing");
  const uint64_t elements = multiply(value->allocRows, channels, operation, operand);
  buffer(&value->buffer, multiply(elements, formatBytes(format), operation, operand), operation, operand);
}

inline void residual(const Activation* value, uint32_t rows, uint32_t channels,
                     const char* operation) {
  require(value != nullptr, operation, "residual", "is missing");
  require(value->format == Format::E4 || value->format == Format::F16,
          operation, "residual", "must be E4 or F16");
  activation(value, value->format, rows, channels, operation, "residual");
}

inline void auxiliary(const Tensor* value, uint32_t offset, uint32_t bytes,
                      uint32_t alignment, bool wordReads, const char* operation,
                      const char* operand) {
  require(value != nullptr, operation, operand, "tensor is missing");
  require(offset % alignment == 0, operation, operand, "offset is misaligned");
  const uint64_t end = uint64_t(offset) + bytes;
  // Real model tensors carry a logical length. Synthetic callers may provide
  // only raw storage; the GPU allocation remains mandatory in both cases.
  require(value->byteLength == 0 || end <= value->byteLength,
          operation, operand, "range exceeds the model tensor");
  // The block shader extracts halves from uint words. The final half can need
  // two allocation-padding bytes even though those are not logical model data.
  buffer(&value->raw, wordReads ? (end + 3u) & ~uint64_t(3u) : end,
         operation, operand);
}

inline uint32_t geometry(uint32_t width, uint32_t height, uint32_t shiftX,
                         uint32_t shiftY, const char* operation) {
  require(width != 0 && height != 0, operation, "geometry", "must be nonzero");
  require(shiftX < 8 && shiftY < 8, operation, "window shift", "must be within an 8x8 tile");
  // Window origins and validity predicates use signed GLSL coordinates.
  constexpr uint32_t limit = uint32_t(std::numeric_limits<int32_t>::max()) - 14u;
  require(width <= limit && height <= limit, operation, "geometry", "exceeds signed window coordinates");
  const uint64_t rows = uint64_t(width) * height;
  require(rows <= std::numeric_limits<uint32_t>::max() / 32u,
          operation, "geometry", "exceeds shader uint indexing");
  const uint64_t windows = ((uint64_t(width) + shiftX + 7u) / 8u) *
                           ((uint64_t(height) + shiftY + 7u) / 8u);
  require(windows != 0 && windows <= std::numeric_limits<uint32_t>::max() - 65534u,
          operation, "geometry", "window dispatch overflows");
  return uint32_t(rows);
}

inline void noWriteAlias(std::initializer_list<const vk::Buffer*> writes,
                         std::initializer_list<const vk::Buffer*> reads,
                         const char* operation) {
  for (const auto* output : writes) {
    if (!output) continue;
    for (const auto* input : reads)
      require(!input || output->buffer != input->buffer,
              operation, "output", "aliases a read operand");
  }
  for (auto output = writes.begin(); output != writes.end(); ++output)
    for (auto other = output + 1; other != writes.end(); ++other)
      require(!*output || !*other || (*output)->buffer != (*other)->buffer,
              operation, "outputs", "alias one another");
  // Buffer has no suballocation-offset metadata. The caller must additionally
  // ensure distinct handles do not overlap a VkDeviceMemory suballocation.
}

}  // namespace amd_fusion_detail

inline void validateAmdFfn32(const AmdFfn32Args& a) {
  using namespace amd_fusion_detail;
  constexpr const char* op = "AMD C32 FFN";
  activation(a.input, Format::E4, a.rows, 32, op, "input");
  residual(a.residual, a.rows, 32, op);
  activation(a.rawOutput, Format::F16, a.rows, 32, op, "raw output");
  activation(a.quantizedOutput, Format::E4, a.rows, 32, op, "E4 output");
  buffer(a.expandWeights, 32u * 128u, op, "expand weights");
  buffer(a.contractWeights, 128u * 32u, op, "contract weights");
  auxiliary(a.auxTensor, a.scaleByteOffset, 32u * 2u, 2, false, op, "FFN skip scale");
  noWriteAlias({&a.rawOutput->buffer, &a.quantizedOutput->buffer},
               {&a.input->buffer, &a.residual->buffer, a.expandWeights,
                a.contractWeights, &a.auxTensor->raw}, op);
}

inline void validateAmdQkv32(const AmdQkv32Args& a) {
  using namespace amd_fusion_detail;
  constexpr const char* op = "AMD C32 QKV";
  const uint32_t rows = geometry(a.width, a.height, a.shiftX, a.shiftY, op);
  activation(a.input, Format::E4, rows, 32, op, "input");
  activation(a.attended, Format::E4, rows, 32, op, "attended output");
  buffer(a.weights, 32u * 96u, op, "QKV weights");
  buffer(a.prior, 64u * 64u * 2u, op, "attention prior");
  auxiliary(a.auxTensor, a.scaleByteOffset, 4, 4, true, op, "Q scale");
  noWriteAlias({&a.attended->buffer},
               {&a.input->buffer, a.weights, a.prior, &a.auxTensor->raw}, op);
}

inline void validateAmdExpertFfn(const AmdExpertFfnArgs& a) {
  using namespace amd_fusion_detail;
  constexpr const char* op = "AMD expert FFN";
  require(a.channels == 64 || a.channels == 128 || a.channels == 256,
          op, "channels", "must be 64, 128 or 256");
  activation(a.input, Format::E4, a.rows, a.channels, op, "input");
  residual(a.residual, a.rows, a.channels, op);
  activation(a.rawOutput, Format::F16, a.rows, a.channels, op, "raw output");
  activation(a.quantizedOutput, Format::E4, a.rows, a.channels, op, "E4 output");
  const uint64_t experts = a.channels / 32u;
  buffer(a.expandWeights, experts * a.channels * 128u, op, "expand weights");
  buffer(a.narrowWeights, experts * 128u * 32u, op, "narrow weights");
  buffer(a.projectWeights, uint64_t(a.channels) * a.channels, op, "projection weights");
  auxiliary(a.auxTensor, a.scaleByteOffset, a.channels * 2u, 2, false, op, "FFN skip scale");
  noWriteAlias({&a.rawOutput->buffer, &a.quantizedOutput->buffer},
               {&a.input->buffer, &a.residual->buffer, a.expandWeights,
                a.narrowWeights, a.projectWeights, &a.auxTensor->raw}, op);
}

inline void validateAmdBlock32(const AmdBlock32Args& a) {
  using namespace amd_fusion_detail;
  constexpr const char* op = "AMD C32 block";
  const uint32_t rows = geometry(a.width, a.height, a.shiftX, a.shiftY, op);
  activation(a.state, Format::E4, rows, 32, op, "state");
  residual(a.residual, rows, 32, op);
  require(a.output || a.rawOutput, op, "outputs", "are both missing");
  if (a.output) activation(a.output, Format::E4, rows, 32, op, "E4 output");
  if (a.rawOutput) activation(a.rawOutput, Format::F16, rows, 32, op, "raw output");
  buffer(a.expandWeights, 32u * 128u, op, "expand weights");
  buffer(a.contractWeights, 128u * 32u, op, "contract weights");
  buffer(a.qkvWeights, 32u * 96u, op, "QKV weights");
  buffer(a.projectionWeights, 32u * 32u, op, "projection weights");
  buffer(a.prior, 64u * 64u * 2u, op, "attention prior");
  auxiliary(a.auxTensor, a.ffnScaleByteOffset, 32u * 2u, 2, true, op, "FFN skip scale");
  auxiliary(a.auxTensor, a.attentionScaleByteOffset, 32u * 2u, 2, true, op, "attention skip scale");
  auxiliary(a.auxTensor, a.qScaleByteOffset, 4, 4, true, op, "Q scale");
  noWriteAlias({a.output ? &a.output->buffer : nullptr,
                a.rawOutput ? &a.rawOutput->buffer : nullptr},
               {&a.state->buffer, &a.residual->buffer, a.expandWeights,
                a.contractWeights, a.qkvWeights, a.projectionWeights,
                a.prior, &a.auxTensor->raw}, op);
}

}  // namespace nr
