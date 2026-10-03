// Preserving mixed publication spelling of common.glsl mpCubicSilu.
// Keep its F32 operation order, explicit FMAs and all five half boundaries.
#ifndef NR_AMD_SILU_RTE_GLSL
#define NR_AMD_SILU_RTE_GLSL
#include "common.glsl"
#include "amd_half_publication_rte.glsl"
// The F32 multiply/FMA feeding a half publication must retain signed zero.
// Half-only modes do not prevent LLPC from discarding the final negative zero.
// The build's execution-mode closure must retain both F16 and F32 declarations;
// glslang's intrinsic otherwise keeps only the last width of this mode enum.
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4466], 4461, 32);
float nrCubicSiluRte(float value) {
  precise float bounded = nrHalfPublishRte(clamp(value, -4.0, 4.0));
  precise float absolute = nrHalfPublishRte(abs(bounded));
  // Scalar-RTE here lets the tested LLPC driver narrow the F32 FMA before its
  // required intermediate rounding. Keep the original software publication.
  precise float inner = roundF16(fma(-0.055908203125, absolute, 0.447265625));
  precise float polynomial = nrHalfPublishRte(fma(bounded, inner, 0.89453125));
  // The tested LLPC driver also discards the multiply's negative zero through
  // scalar-RTE despite the declared controls. Original publication retains it.
  return roundF16(value * polynomial);
}
#endif
