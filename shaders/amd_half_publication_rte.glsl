// Scalar-RTE half publication for AMD RTE modules and diagnostic probes.
// Unlike GLSL.std.450 PackHalf2x16, the scalar OpFConvert must honor the
// explicit RoundingModeRTE execution mode. Keeping the half bitcast makes
// the intermediate publication observable in the diagnostic output.
#ifndef OPEN_NR_AMD_HALF_PUBLICATION_RTE
#define OPEN_NR_AMD_HALF_PUBLICATION_RTE
#extension GL_EXT_spirv_intrinsics : require
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4467], 4462, 16);
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4464], 4459, 16);
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4466], 4461, 16);

uint nrHalfBitsRte(float value) {
  return uint(float16BitsToUint16(float16_t(value)));
}

float nrHalfPublishRte(float value) {
  // Existing roundF16 returns Inf/NaN unchanged, including the NaN payload.
  // Do not send a NaN through the half conversion: its payload is not stable.
  if ((floatBitsToUint(value) & 0x7fffffffu) >= 0x7f800000u) return value;
  return float(uint16BitsToFloat16(uint16_t(nrHalfBitsRte(value))));
}
#endif
