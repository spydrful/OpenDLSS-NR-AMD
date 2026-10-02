#include "portable_numeric.glsl"
layout(local_size_x = 128) in;
layout(constant_id = 0) const uint K = 32;
layout(constant_id = 2) const uint FLAGS = 16;
layout(constant_id = 3) const uint PARTITION = 0;
layout(push_constant) uniform Push {
  uint rows, N, Nmatrix, weightColumnOffset, inputStride, inputColumnBase;
  uint outputStride, outputColumnOffset, auxHalfOffset, batches, columnGroups, splitStride;
} pc;
layout(std430, binding = 0) readonly buffer A { uint8_t a[]; };
layout(std430, binding = 1) readonly buffer B { uint8_t b[]; };
layout(std430, binding = 2) writeonly buffer O16 { float16_t out16[]; };
layout(std430, binding = 3) readonly buffer R16 { float16_t res16[]; };
layout(std430, binding = 4) readonly buffer Aux { float16_t aux[]; };
layout(std430, binding = 5) writeonly buffer O8 { uint8_t out8[]; };
layout(std430, binding = 6) readonly buffer R8 { uint8_t res8[]; };
shared uint8_t sa[64 * 16];
shared uint8_t sb[16 * 16];
#ifdef NR_AMD_MATRIX
shared float sc[64 * 16];
#define ACC coopmat<float, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define MA coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define MB coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#endif
float initial(uint row, uint column, uint batch) {
  if ((FLAGS & 1u) == 0u || row >= pc.rows) return 0.0;
  uint index = row * pc.outputStride + pc.outputColumnOffset + batch * pc.N + column;
  float value = (FLAGS & 64u) != 0u ? nrDecodeE4(uint(res8[index])) : float(res16[index]);
  if ((FLAGS & 2u) != 0u) value *= float(aux[pc.auxHalfOffset + column]);
  return roundF16(value);
}
void publish(uint row, uint column, uint batch, float value) {
  if (row >= pc.rows || column >= pc.N) return;
  if ((FLAGS & 8u) != 0u) value = mpCubicSilu(value);
  uint index = row * pc.outputStride + pc.outputColumnOffset + batch * pc.N + column;
  if ((FLAGS & 16u) == 0u) out16[index] = float16_t(value);
  if ((FLAGS & 48u) != 0u) out8[index] = uint8_t(nrPublishE4(value));
}
void main() {
  uint thread = gl_LocalInvocationID.x;
  uint batch = gl_WorkGroupID.x / pc.columnGroups;
  uint colBase = (gl_WorkGroupID.x % pc.columnGroups) * 16u;
  uint rowBase = (gl_WorkGroupID.y + gl_WorkGroupID.z * 65535u) * 64u;
  uint inBase = pc.inputColumnBase + ((FLAGS & 128u) != 0u ? 0u : batch * K);
#ifdef NR_AMD_MATRIX
  for (uint i = thread; i < 1024u; i += 128u) sc[i] = initial(rowBase + i / 16u, colBase + i % 16u, batch);
  barrier();
  uint sg = gl_SubgroupID;
  ACC acc, total = ACC(0.0);
  coopMatLoad(acc, sc, sg * 256u, 16u, gl_CooperativeMatrixLayoutRowMajor);
#else
  float sums[8], totals[8];
  for (uint j = 0u; j < 8u; ++j) {
    uint i = thread + j * 128u;
    sums[j] = initial(rowBase + i / 16u, colBase + i % 16u, batch); totals[j] = 0.0;
  }
#endif
  for (uint kb = 0u; kb < K; kb += 16u) {
    for (uint i = thread; i < 1024u; i += 128u) {
      uint row = rowBase + i / 16u;
      sa[i] = row < pc.rows ? a[row * pc.inputStride + inBase + kb + i % 16u] : uint8_t(0);
    }
    for (uint i = thread; i < 256u; i += 128u) {
      uint c = colBase + i / 16u, k = kb + i % 16u;
      uint index = ((batch * (K / 32u) + k / 32u) * pc.Nmatrix + pc.weightColumnOffset + c) * 32u + k % 32u;
      sb[i] = c < pc.N ? b[index] : uint8_t(0);
    }
    barrier();
#ifdef NR_AMD_MATRIX
    MA ma; MB mb;
    coopMatLoad(ma, sa, sg * 256u, 16u, gl_CooperativeMatrixLayoutRowMajor);
    coopMatLoad(mb, sb, 0u, 16u, gl_CooperativeMatrixLayoutColumnMajor);
    acc = coopMatMulAdd(ma, mb, acc);
    // AMD accumulates in f32. Retain the network's half publication points;
    // F13 truncation remains exclusive to the exact software route.
    for (uint i = 0u; i < acc.length(); ++i) acc[i] = roundF16(acc[i]);
    if (PARTITION != 0u && (kb + 16u) % PARTITION == 0u) {
      if (kb < PARTITION) total = acc;
      else for (uint i = 0u; i < acc.length(); ++i) total[i] = roundF16(total[i] + acc[i]);
      acc = ACC(0.0);
    }
#else
    for (uint j = 0u; j < 8u; ++j) {
      uint i = thread + j * 128u; float av[16], bv[16];
      for (uint k = 0u; k < 16u; ++k) {
        av[k] = nrDecodeE4(uint(sa[(i / 16u) * 16u + k]));
        bv[k] = nrDecodeE4(uint(sb[(i % 16u) * 16u + k]));
      }
      sums[j] = nrFdpa16(av, bv, sums[j]);
      if (PARTITION != 0u && (kb + 16u) % PARTITION == 0u) {
        totals[j] = kb < PARTITION ? sums[j] : roundF16(totals[j] + sums[j]); sums[j] = 0.0;
      }
    }
#endif
    barrier();
  }
#ifdef NR_AMD_MATRIX
  if (PARTITION != 0u) acc = total;
  coopMatStore(acc, sc, sg * 256u, 16u, gl_CooperativeMatrixLayoutRowMajor);
  barrier();
  for (uint i = thread; i < 1024u; i += 128u) publish(rowBase + i / 16u, colBase + i % 16u, batch, sc[i]);
#else
  for (uint j = 0u; j < 8u; ++j) {
    uint i = thread + j * 128u;
    publish(rowBase + i / 16u, colBase + i % 16u, batch, PARTITION != 0u ? totals[j] : sums[j]);
  }
#endif
}
