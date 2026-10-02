#include "portable_numeric.glsl"
layout(local_size_x = 32) in;
layout(constant_id = 0) const uint PADDED_TOKENS = 64;
layout(push_constant) uniform Push { uint tokens, heads, channels; } pc;
layout(std430, binding = 0) readonly buffer Qkv { uint8_t qkv[]; };
layout(std430, binding = 5) writeonly buffer Output { uint8_t attended[]; };
shared float16_t scores[PADDED_TOKENS];
shared uint8_t weights[PADDED_TOKENS];
shared float reciprocal;
float dot16(float a[16], float b[16], float acc) {
#ifdef NR_FAST_DOT
  for (uint i = 0u; i < 16u; ++i) acc = fma(a[i], b[i], acc);
  return roundF16(acc);
#else
  return nrFdpa16(a, b, acc);
#endif
}
float pairSum(uint base, uint pair, uint parity) {
  uint k = base + pair * 2u + parity;
  float a = roundF16(float(scores[k]) + float(scores[k + 8u]));
  float b = roundF16(float(scores[k + 16u]) + float(scores[k + 24u]));
  float c = roundF16(float(scores[k + 32u]) + float(scores[k + 40u]));
  float d = roundF16(float(scores[k + 48u]) + float(scores[k + 56u]));
  return roundF16(roundF16(roundF16(a + b) + c) + d);
}
float softmax64(uint base) {
  float e = roundF16(pairSum(base, 0u, 0u) + pairSum(base, 1u, 0u));
  e = roundF16(roundF16(e + pairSum(base, 2u, 0u)) + pairSum(base, 3u, 0u));
  float o = roundF16(pairSum(base, 0u, 1u) + pairSum(base, 1u, 1u));
  o = roundF16(roundF16(o + pairSum(base, 2u, 1u)) + pairSum(base, 3u, 1u));
  return roundF16(e + o);
}
void main() {
  uint lane = gl_LocalInvocationID.x, head = gl_WorkGroupID.x;
  uint query = gl_WorkGroupID.y + gl_WorkGroupID.z * 65535u;
  if (query >= pc.tokens) return;
  uint stride = pc.channels * 3u, headBase = head * 96u;
  for (uint key = lane; key < PADDED_TOKENS; key += 32u) {
    float acc = 0.0;
    for (uint kb = 0u; kb < 32u; kb += 16u) {
      float a[16], b[16];
      for (uint k = 0u; k < 16u; ++k) {
        a[k] = nrDecodeE4(uint(qkv[query * stride + headBase + kb + k]));
        b[k] = key < pc.tokens ? nrDecodeE4(uint(qkv[key * stride + headBase + 32u + kb + k])) : 0.0;
      }
      acc = dot16(a, b, acc);
    }
    scores[key] = float16_t(nrVitExp(acc));
    weights[key] = uint8_t(nrPublishE4(float(scores[key])));
  }
  barrier();
  if (lane == 0u) {
    float total = 0.0;
    for (uint base = 0u; base < PADDED_TOKENS; base += 64u) total = roundF16(total + softmax64(base));
    if (PADDED_TOKENS > pc.tokens) total = roundF16(total - roundF16(nrVitExp(0.0) * float(PADDED_TOKENS - pc.tokens)));
    reciprocal = roundF16(1.0 / total);
  }
  barrier();
  float acc = 0.0;
  for (uint kb = 0u; kb < PADDED_TOKENS; kb += 16u) {
    float a[16], b[16];
    for (uint k = 0u; k < 16u; ++k) {
      a[k] = nrDecodeE4(uint(weights[kb + k]));
      b[k] = kb + k < pc.tokens ? nrDecodeE4(uint(qkv[(kb + k) * stride + headBase + 64u + lane])) : 0.0;
    }
    acc = dot16(a, b, acc);
  }
  attended[query * pc.channels + head * 32u + lane] = uint8_t(nrPublishE4(roundF16(acc * reciprocal)));
}
