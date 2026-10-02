// AMD fusion operand contracts. The native reference and NVIDIA/PTX paths keep
// their existing contracts; selecting these kernels is an independent option.
#pragma once
#include "vk_context.h"

namespace nr {
struct Activation;
struct Tensor;

struct AmdFfn32Args {
  const Activation* input = nullptr;       // E4 [rows][32]
  const Activation* residual = nullptr;    // E4 or raw F16 [rows][32]
  const vk::Buffer* expandWeights = nullptr;    // loader fp8Matrix, K32/N128
  const vk::Buffer* contractWeights = nullptr;  // loader fp8Matrix, K128/N32
  const Tensor* auxTensor = nullptr;
  Activation* rawOutput = nullptr;         // raw F16 residual-seeded contraction
  Activation* quantizedOutput = nullptr;   // E4 twin of the same contraction
  uint32_t rows = 0;
  uint32_t scaleByteOffset = 0;
};

struct AmdQkv32Args {
  const Activation* input = nullptr;       // E4 [width*height][32]
  const vk::Buffer* weights = nullptr;     // loader fp8Matrix, K32/N96
  const vk::Buffer* prior = nullptr;       // loader relativeBias, half [64][64]
  const Tensor* auxTensor = nullptr;
  Activation* attended = nullptr;         // E4 [width*height][32]
  uint32_t scaleByteOffset = 0;            // one f32 learned Q scale
  uint32_t width = 0, height = 0, shiftX = 0, shiftY = 0;
};

struct AmdExpertFfnArgs {
  const Activation* input = nullptr;       // E4 [rows][C], C64/C128/C256
  const Activation* residual = nullptr;    // E4 or F16 scaled skip
  const vk::Buffer* expandWeights = nullptr;   // loader fp8Matrix, [E*C][128], batchK=C
  const vk::Buffer* narrowWeights = nullptr;   // loader fp8Matrix, [E*128][32], batchK=128
  const vk::Buffer* projectWeights = nullptr;  // loader fp8Matrix, [C][C]
  const Tensor* auxTensor = nullptr;
  Activation* rawOutput = nullptr;         // F16 twin; not used as the attention skip
  Activation* quantizedOutput = nullptr;   // E4, also the expert attention skip
  uint32_t rows = 0, channels = 0, scaleByteOffset = 0;
};

struct AmdBlock32Args {
  const Activation* state = nullptr;       // E4 input, with external adapters already applied
  const Activation* residual = nullptr;    // E4 or raw F16 FFN skip
  const vk::Buffer* expandWeights = nullptr;
  const vk::Buffer* contractWeights = nullptr;
  const vk::Buffer* qkvWeights = nullptr;
  const vk::Buffer* projectionWeights = nullptr;
  const vk::Buffer* prior = nullptr;
  const Tensor* auxTensor = nullptr;
  Activation* output = nullptr;            // optional E4 block publication; block70 needs only rawOutput
  Activation* rawOutput = nullptr;         // optional F16 block output for external transitions/head
  uint32_t ffnScaleByteOffset = 0, attentionScaleByteOffset = 0, qScaleByteOffset = 0;
  uint32_t width = 0, height = 0, shiftX = 0, shiftY = 0;
};
}  // namespace nr
