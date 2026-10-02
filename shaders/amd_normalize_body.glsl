// Sixteen lanes cooperate on one 32-channel head. This keeps Q/K/V accesses
// contiguous and avoids the original per-thread arrays of 64 half values.
// XOR distances reproduce the specified 8/4/2/1 half reduction tree.
#define DLSS_E4M3_HW 1
#include "common.glsl"
layout(local_size_x = 256) in;
layout(push_constant) uniform Push { uint tokens, heads, channels, scaleWordOffset; } pc;
layout(std430, binding = 1) readonly buffer Qkv { float16_t qkv[]; };
layout(std430, binding = 4) readonly buffer Aux { uint aux32[]; };
layout(std430, binding = 5) writeonly buffer Normalized { uint8_t normalized[]; };
void main() {
  uint invocation = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * 65535u * 256u;
  uint id = invocation / 16u, component = invocation & 15u;
  if (id >= pc.tokens * pc.heads) return;
  uint token = id / pc.heads, head = id % pc.heads;
  uint base = token * pc.channels * 3u + head * 96u;
  float16_t q0 = qkv[base + component], q1 = qkv[base + 16u + component];
  float16_t k0 = qkv[base + 32u + component], k1 = qkv[base + 48u + component];
  precise float16_t qHigh = q1 * q1, kHigh = k1 * k1;
#ifdef NR_GLOBAL_NORMALIZE
  precise float qPair = float(q0) * float(q0) + float(qHigh);
  precise float kPair = float(k0) * float(k0) + float(kHigh);
  float16_t qSum = float16_t(qPair), kSum = float16_t(kPair);
#else
  float16_t qSum = fma(q0, q0, qHigh), kSum = fma(k0, k0, kHigh);
#endif
  for (uint distance = 8u; distance != 0u; distance >>= 1u) {
    precise float16_t qNext = qSum + float16_t(subgroupShuffleXor(float(qSum), distance));
    precise float16_t kNext = kSum + float16_t(subgroupShuffleXor(float(kSum), distance));
    qSum = qNext; kSum = kNext;
  }
  float16_t qNorm = float16_t(inversesqrt(float(qSum)));
  float16_t kNorm = float16_t(inversesqrt(float(kSum)));
  float16_t scale = float16_t(uintBitsToFloat(aux32[pc.scaleWordOffset + head]));
  precise float16_t nq0 = q0 * qNorm, nq1 = q1 * qNorm;
#ifdef NR_GLOBAL_NORMALIZE
  precise float16_t qs0 = nq0 * float16_t(5.65625), qs1 = nq1 * float16_t(5.65625);
  nq0 = qs0; nq1 = qs1;
#endif
  precise float16_t nqs0 = nq0 * scale, nqs1 = nq1 * scale;
  precise float16_t nk0 = k0 * kNorm, nk1 = k1 * kNorm;
  normalized[base + component] = e4m3Hw(nqs0);
  normalized[base + 16u + component] = e4m3Hw(nqs1);
  normalized[base + 32u + component] = e4m3Hw(nk0);
  normalized[base + 48u + component] = e4m3Hw(nk1);
  normalized[base + 64u + component] = e4m3Hw(qkv[base + 64u + component]);
  normalized[base + 80u + component] = e4m3Hw(qkv[base + 80u + component]);
}
