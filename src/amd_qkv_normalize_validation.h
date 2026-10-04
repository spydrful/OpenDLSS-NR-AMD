#pragma once
// Independent QKV GEMM -> normalization route. The older QKV -> attention
// fusion and its shorthand flags retain their original contracts.
#include "amd_config.h"
#include "vk_context.h"

namespace amd {
inline bool qkvNormalizeSupported(const vk::DeviceCapabilities& caps) {
  // Context device selection already requires controllable compute wave32,
  // full subgroups and 8/16-bit storage/arithmetic before exposing these caps.
  return caps.backend==vk::Backend::AmdFast && caps.fp8Matrix16 && caps.halfPublicationRte &&
      caps.float32SignedZeroInfNan && (caps.subgroupOperations & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) &&
      caps.properties.limits.maxComputeWorkGroupInvocations>=256 &&
      caps.properties.limits.maxComputeWorkGroupSize[0]>=256 &&
      caps.properties.limits.maxComputeSharedMemorySize>=9216;
}
inline bool qkvNormalizeBlock(int block) {
  return block==0 || block==1 || block==2 || block==3 || block==4 ||
      block==66 || block==67 || block==68 || block==69 || block==70;
}
inline bool qkvNormalizeLayout(int block,uint32_t weights,uint32_t scale) {
  if(block==0)return weights==9312 && scale==20576;
  if(block==66)return weights==10400 && scale==21664;
  if(block==70)return weights==8400 && scale==19664;
  return qkvNormalizeBlock(block) && weights==8288 && scale==19552;
}
inline bool qkvNormalizeAddressableRows(uint32_t rows) {
  return rows>0 && rows<=UINT32_MAX-63u && ((uint64_t(rows)+63u)&~uint64_t(63u))*96u<=UINT32_MAX;
}
constexpr const char* qkvNormalizeShader="amd_qkv32_normalize_wave6";
constexpr const char* qkvNormalizeShaderSha256="9b1c585054bbbe3c9c2021d393699eae16e9ffcf889db6311d070d0049be5cc5";
constexpr const char* qkvNormalizeModelSha256="163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e";
} // namespace amd
