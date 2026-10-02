#include "portable_numeric.glsl"
layout(local_size_x = 128) in;
layout(push_constant) uniform Push { uint width, height, channels, heads, shiftX, shiftY, windowsX, windowCount; } pc;
layout(std430, binding = 0) readonly buffer Qkv { uint8_t qkv[]; };
layout(std430, binding = 1) readonly buffer Prior { float16_t prior[]; };
layout(std430, binding = 5) writeonly buffer Output { uint8_t attended[]; };
shared uint8_t qs[2048], ks[2048], vs[2048], weights[4096];
shared float16_t scores[4096];
#ifdef NR_AMD_MATRIX
shared float scratch[4096];
#define ACC coopmat<float, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define MA coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define MB coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#endif
uint untile(uint t) { uint tile = t >> 4u, local = t & 15u; return ((tile >> 1u) * 4u + (local >> 2u)) * 8u + (tile & 1u) * 4u + (local & 3u); }
float pairSum(uint row, uint pair, uint parity) {
  uint k = row * 64u + pair * 2u + parity;
  float a = roundF16(float(scores[k]) + float(scores[k + 8u]));
  float b = roundF16(float(scores[k + 16u]) + float(scores[k + 24u]));
  float c = roundF16(float(scores[k + 32u]) + float(scores[k + 40u]));
  float d = roundF16(float(scores[k + 48u]) + float(scores[k + 56u]));
  return roundF16(roundF16(roundF16(a + b) + c) + d);
}
float softmaxSum(uint row) {
  float even = roundF16(pairSum(row, 0u, 0u) + pairSum(row, 1u, 0u));
  even = roundF16(roundF16(even + pairSum(row, 2u, 0u)) + pairSum(row, 3u, 0u));
  float odd = roundF16(pairSum(row, 0u, 1u) + pairSum(row, 1u, 1u));
  odd = roundF16(roundF16(odd + pairSum(row, 2u, 1u)) + pairSum(row, 3u, 1u));
  return roundF16(even + odd);
}
void main() {
  uint thread = gl_LocalInvocationID.x, head = gl_WorkGroupID.x;
  uint window = gl_WorkGroupID.y + gl_WorkGroupID.z * 65535u;
  if (window >= pc.windowCount) return;
  int wx = int((window % pc.windowsX) * 8u) - int(pc.shiftX);
  int wy = int((window / pc.windowsX) * 8u) - int(pc.shiftY);
  for (uint i = thread; i < 2048u; i += 128u) {
    uint t = i / 32u, c = i % 32u;
    int x = wx + int(t % 8u), y = wy + int(t / 8u);
    qs[i] = uint8_t(0); ks[i] = uint8_t(0); vs[i] = uint8_t(0);
    if (x >= 0 && y >= 0 && x < int(pc.width) && y < int(pc.height)) qs[i] = qkv[(uint(y) * pc.width + uint(x)) * pc.channels * 3u + head * 96u + c];
    uint key = untile(t); x = wx + int(key % 8u); y = wy + int(key / 8u);
    if (x >= 0 && y >= 0 && x < int(pc.width) && y < int(pc.height)) {
      uint base = (uint(y) * pc.width + uint(x)) * pc.channels * 3u + head * 96u;
      ks[i] = qkv[base + 32u + c]; vs[i] = qkv[base + 64u + c];
    }
  }
#ifdef NR_AMD_MATRIX
  for (uint i = thread; i < 4096u; i += 128u) scratch[i] = float(prior[head * 4096u + i]);
#endif
  barrier();
#ifdef NR_AMD_MATRIX
  for (uint tile = gl_SubgroupID; tile < 16u; tile += 4u) {
    uint qt = tile / 4u, kt = tile % 4u;
    ACC acc; coopMatLoad(acc, scratch, qt * 1024u + kt * 16u, 64u, gl_CooperativeMatrixLayoutRowMajor);
    for (uint kb = 0u; kb < 32u; kb += 16u) {
      MA a; MB b;
      coopMatLoad(a, qs, qt * 512u + kb, 32u, gl_CooperativeMatrixLayoutRowMajor);
      coopMatLoad(b, ks, kt * 512u + kb, 32u, gl_CooperativeMatrixLayoutColumnMajor);
      acc = coopMatMulAdd(a, b, acc);
      for (uint i = 0u; i < acc.length(); ++i) acc[i] = roundF16(acc[i]);
    }
    coopMatStore(acc, scratch, qt * 1024u + kt * 16u, 64u, gl_CooperativeMatrixLayoutRowMajor);
  }
  barrier();
  for (uint i = thread; i < 4096u; i += 128u) scores[i] = float16_t(nrWindowExp(scratch[i]));
#else
  for (uint i = thread; i < 4096u; i += 128u) {
    uint q = i / 64u, key = i % 64u; float acc = float(prior[head * 4096u + i]);
    for (uint kb = 0u; kb < 32u; kb += 16u) {
      float a[16], b[16];
      for (uint k = 0u; k < 16u; ++k) { a[k] = nrDecodeE4(uint(qs[q * 32u + kb + k])); b[k] = nrDecodeE4(uint(ks[key * 32u + kb + k])); }
      acc = nrFdpa16(a, b, acc);
    }
    scores[i] = float16_t(nrWindowExp(acc));
  }
#endif
  barrier();
  if (thread < 64u) {
    float reciprocal = roundF16(1.0 / softmaxSum(thread));
    for (uint key = 0u; key < 64u; ++key) weights[thread * 64u + key] = uint8_t(nrPublishE4(roundF16(float(scores[thread * 64u + key]) * reciprocal)));
  }
  barrier();
#ifdef NR_AMD_MATRIX
  for (uint tile = gl_SubgroupID; tile < 8u; tile += 4u) {
    uint qt = tile / 2u, ct = tile % 2u; ACC acc = ACC(0.0);
    for (uint kb = 0u; kb < 64u; kb += 16u) {
      MA a; MB b;
      coopMatLoad(a, weights, qt * 1024u + kb, 64u, gl_CooperativeMatrixLayoutRowMajor);
      coopMatLoad(b, vs, kb * 32u + ct * 16u, 32u, gl_CooperativeMatrixLayoutRowMajor);
      acc = coopMatMulAdd(a, b, acc);
      for (uint i = 0u; i < acc.length(); ++i) acc[i] = roundF16(acc[i]);
    }
    coopMatStore(acc, scratch, qt * 512u + ct * 16u, 32u, gl_CooperativeMatrixLayoutRowMajor);
  }
  barrier();
#endif
  for (uint i = thread; i < 2048u; i += 128u) {
    uint q = i / 32u, c = i % 32u;
    int x = wx + int(q % 8u), y = wy + int(q / 8u);
    if (x < 0 || y < 0 || x >= int(pc.width) || y >= int(pc.height)) continue;
#ifdef NR_AMD_MATRIX
    float acc = scratch[i];
#else
    float acc = 0.0;
    for (uint kb = 0u; kb < 64u; kb += 16u) {
      float a[16], b[16];
      for (uint k = 0u; k < 16u; ++k) { a[k] = nrDecodeE4(uint(weights[q * 64u + kb + k])); b[k] = nrDecodeE4(uint(vs[(kb + k) * 32u + c])); }
      acc = nrFdpa16(a, b, acc);
    }
#endif
    attended[(uint(y) * pc.width + uint(x)) * pc.channels + head * 32u + c] = uint8_t(nrPublishE4(acc));
  }
}
