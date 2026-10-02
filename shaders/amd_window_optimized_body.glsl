// Compact AMD window attention. Requires 128 invocations, four wave32 subgroups.
// Default arithmetic matches amd_window: FP32 MMA with half publication at K16.
// No accumulator-component-to-coordinate mapping is assumed: epilogues use a
// 16x16 RowMajor tile private to each subgroup.
#ifndef NR_AMD_WINDOW_OPTIMIZED_BODY
#define NR_AMD_WINDOW_OPTIMIZED_BODY
#include "portable_numeric.glsl"

// 16 preserves the baseline; 32 and 0 are separately validated arithmetic modes.
// Every product's final K16 step publishes half, including interval 0.
layout(constant_id = 10) const uint NR_AW_ROUND_INTERVAL = 16u;

#ifndef NR_AMD_WINDOW_EXTERNAL_INTERFACE
layout(local_size_x = 128) in;
layout(push_constant) uniform Push { uint width, height, channels, heads, shiftX, shiftY, windowsX, windowCount; } pc;
layout(std430, binding = 0) readonly buffer Qkv { uint8_t qkv[]; };
layout(std430, binding = 1) readonly buffer Prior { float16_t prior[]; };
layout(std430, binding = 5) writeonly buffer Output { uint8_t attended[]; };
#endif

// 6 KiB operands + 4 KiB weights + 8 KiB scores + 4 KiB tile scratch = 22 KiB.
shared uint8_t nrAwQs[2048], nrAwKs[2048], nrAwVs[2048], nrAwWeights[4096];
shared float16_t nrAwScores[4096];
shared float nrAwScratch[1024];
#define NR_AW_ACC coopmat<float, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define NR_AW_MA coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define NR_AW_MB coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>

#ifndef NR_AW_STORE_ATTENDED
#define NR_AW_STORE_ATTENDED(x, y, head, channel, value) \
  attended[(uint(y) * pc.width + uint(x)) * pc.channels + (head) * 32u + (channel)] = (value)
#endif

uint nrAwUntile(uint t) {
  uint tile = t >> 4u, local = t & 15u;
  return ((tile >> 1u) * 4u + (local >> 2u)) * 8u + (tile & 1u) * 4u + (local & 3u);
}

bool nrAwPublishAccumulator(uint completed, uint depth) {
  return completed == depth || NR_AW_ROUND_INTERVAL == 16u ||
         (NR_AW_ROUND_INTERVAL == 32u && (completed & 31u) == 0u);
}

void nrAwWaveBarrier() {
  subgroupMemoryBarrierShared();
  subgroupBarrier();
}

// Eight lanes own one query. Lane k computes precisely the baseline branch
// pairSum(row, k/2, k%2). Fixed XOR gathers retain the left-associated half
// additions in both the even and odd chains; this is not a generic reduction.
float nrAwReciprocal(uint row, uint lane) {
  uint k = row * 64u + lane;
  float a = roundF16(float(nrAwScores[k]) + float(nrAwScores[k + 8u]));
  float b = roundF16(float(nrAwScores[k + 16u]) + float(nrAwScores[k + 24u]));
  float c = roundF16(float(nrAwScores[k + 32u]) + float(nrAwScores[k + 40u]));
  float d = roundF16(float(nrAwScores[k + 48u]) + float(nrAwScores[k + 56u]));
  float piece = roundF16(roundF16(roundF16(a + b) + c) + d);
  float p2 = subgroupShuffleXor(piece, 2u);
  float p4 = subgroupShuffleXor(piece, 4u);
  float p6 = subgroupShuffleXor(piece, 6u);
  float parity = roundF16(roundF16(roundF16(piece + p2) + p4) + p6);
  float even = subgroupShuffleXor(parity, lane);
  float odd = subgroupShuffleXor(parity, lane ^ 1u);
  float reciprocal = 0.0;
  if (lane == 0u) reciprocal = roundF16(1.0 / roundF16(even + odd));
  return subgroupShuffleXor(reciprocal, lane);
}

// The caller supplies normalized E4 Q in natural query order and K/V in
// nrAwUntile order, including zero operands for OOB tokens. All invocations
// must enter. Phase barriers cover those staged inputs and a shared epilogue.
void nrAwCompute(uint head, int wx, int wy) {
  uint wave = gl_SubgroupID, lane = gl_SubgroupInvocationID;
  uint tileBase = wave * 256u;
  barrier();

  for (uint tile = wave; tile < 16u; tile += 4u) {
    uint qt = tile / 4u, kt = tile % 4u;
    for (uint i = lane; i < 256u; i += 32u) {
      uint offset = (qt * 16u + i / 16u) * 64u + kt * 16u + i % 16u;
      nrAwScratch[tileBase + i] = float(prior[head * 4096u + offset]);
    }
    nrAwWaveBarrier();
    NR_AW_ACC acc;
    coopMatLoad(acc, nrAwScratch, tileBase, 16u, gl_CooperativeMatrixLayoutRowMajor);
    for (uint kb = 0u; kb < 32u; kb += 16u) {
      NR_AW_MA a; NR_AW_MB b;
      coopMatLoad(a, nrAwQs, qt * 512u + kb, 32u, gl_CooperativeMatrixLayoutRowMajor);
      coopMatLoad(b, nrAwKs, kt * 512u + kb, 32u, gl_CooperativeMatrixLayoutColumnMajor);
      acc = coopMatMulAdd(a, b, acc);
      if (nrAwPublishAccumulator(kb + 16u, 32u))
        for (uint i = 0u; i < acc.length(); ++i) acc[i] = roundF16(acc[i]);
    }
    nrAwWaveBarrier();
    coopMatStore(acc, nrAwScratch, tileBase, 16u, gl_CooperativeMatrixLayoutRowMajor);
    nrAwWaveBarrier();
    for (uint i = lane; i < 256u; i += 32u) {
      uint offset = (qt * 16u + i / 16u) * 64u + kt * 16u + i % 16u;
      nrAwScores[offset] = float16_t(nrWindowExp(nrAwScratch[tileBase + i]));
    }
    nrAwWaveBarrier(); // The next prior tile may overwrite only consumed scratch.
  }
  barrier();

  uint component = lane & 7u;
  for (uint batch = 0u; batch < 4u; ++batch) {
    uint row = wave * 4u + lane / 8u + batch * 16u;
    float reciprocal = nrAwReciprocal(row, component);
    for (uint key = component; key < 64u; key += 8u)
      nrAwWeights[row * 64u + key] = uint8_t(nrPublishE4(roundF16(float(nrAwScores[row * 64u + key]) * reciprocal)));
  }
  barrier();

  for (uint tile = wave; tile < 8u; tile += 4u) {
    uint qt = tile / 2u, ct = tile % 2u;
    NR_AW_ACC acc = NR_AW_ACC(0.0);
    for (uint kb = 0u; kb < 64u; kb += 16u) {
      NR_AW_MA a; NR_AW_MB b;
      coopMatLoad(a, nrAwWeights, qt * 1024u + kb, 64u, gl_CooperativeMatrixLayoutRowMajor);
      coopMatLoad(b, nrAwVs, kb * 32u + ct * 16u, 32u, gl_CooperativeMatrixLayoutRowMajor);
      acc = coopMatMulAdd(a, b, acc);
      if (nrAwPublishAccumulator(kb + 16u, 64u))
        for (uint i = 0u; i < acc.length(); ++i) acc[i] = roundF16(acc[i]);
    }
    coopMatStore(acc, nrAwScratch, tileBase, 16u, gl_CooperativeMatrixLayoutRowMajor);
    nrAwWaveBarrier();
    for (uint i = lane; i < 256u; i += 32u) {
      uint query = qt * 16u + i / 16u, channel = ct * 16u + i % 16u;
      int x = wx + int(query % 8u), y = wy + int(query / 8u);
      if (x >= 0 && y >= 0 && x < int(pc.width) && y < int(pc.height)) {
        uint8_t value = uint8_t(nrPublishE4(nrAwScratch[tileBase + i]));
        NR_AW_STORE_ATTENDED(x, y, head, channel, value);
      }
    }
    nrAwWaveBarrier();
  }
  barrier();
}

#ifndef NR_AMD_WINDOW_EXTERNAL_INTERFACE
void main() {
  uint thread = gl_LocalInvocationID.x, head = gl_WorkGroupID.x;
  uint window = gl_WorkGroupID.y + gl_WorkGroupID.z * 65535u;
  if (window >= pc.windowCount) return;
  int wx = int((window % pc.windowsX) * 8u) - int(pc.shiftX);
  int wy = int((window / pc.windowsX) * 8u) - int(pc.shiftY);
  for (uint i = thread; i < 2048u; i += 128u) {
    uint t = i / 32u, c = i % 32u;
    int x = wx + int(t % 8u), y = wy + int(t / 8u);
    nrAwQs[i] = uint8_t(0); nrAwKs[i] = uint8_t(0); nrAwVs[i] = uint8_t(0);
    if (x >= 0 && y >= 0 && x < int(pc.width) && y < int(pc.height))
      nrAwQs[i] = qkv[(uint(y) * pc.width + uint(x)) * pc.channels * 3u + head * 96u + c];
    uint key = nrAwUntile(t); x = wx + int(key % 8u); y = wy + int(key / 8u);
    if (x >= 0 && y >= 0 && x < int(pc.width) && y < int(pc.height)) {
      uint base = (uint(y) * pc.width + uint(x)) * pc.channels * 3u + head * 96u;
      nrAwKs[i] = qkv[base + 32u + c]; nrAwVs[i] = qkv[base + 64u + c];
    }
  }
  nrAwCompute(head, wx, wy);
}
#endif
#endif
