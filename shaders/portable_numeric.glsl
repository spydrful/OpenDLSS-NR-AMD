// Software spelling of the Ada publication contract, shared by the native
// reference kernels. See ports/browser-webgpu/shaders/numerics.wgsl.
#ifndef NR_PORTABLE_NUMERIC
#define NR_PORTABLE_NUMERIC
#include "common.glsl"
float nrDecodeE4(uint code) {
#ifdef NR_AMD_HARDWARE_NUMERIC
  return e4m3HwToF32(code);
#else
  return e4m3ToF32(code);
#endif
}
uint nrPublishE4(float value) {
#ifdef NR_AMD_HARDWARE_NUMERIC
  return uint(e4m3Hw(float16_t(value)));
#else
  return e4m3CodeFromF32(value);
#endif
}
uint roundShiftRightEven(uint value, uint shift) {
  if (shift == 0u) return value;
  if (shift > 31u) return 0u;
  return shiftRne(value, shift);
}
float nrPow2(int e) { return uintBitsToFloat(uint(clamp(e, -126, 127) + 127) << 23u); }
int nrExponent(float x, int floorExponent) {
  return max(int((floatBitsToUint(abs(x)) >> 23u) & 255u) - 127, floorExponent);
}
float nrFdpa16(float a[16], float b[16], float acc) {
  if ((floatBitsToUint(acc) & 0x7f800000u) == 0x7f800000u) return acc;
  int e = acc != 0.0 ? nrExponent(acc, -14) : -21;
  for (uint i = 0u; i < 16u; ++i)
    if (a[i] != 0.0 && b[i] != 0.0) e = max(e, nrExponent(a[i], -6) + nrExponent(b[i], -6));
  float scale = nrPow2(13 - e);
  int units = int(trunc(acc * scale));
  for (uint i = 0u; i < 16u; ++i) units += int(trunc((a[i] * b[i]) * scale));
  return roundF16(float(units) * nrPow2(e - 13));
}
float nrFixedHalf(int units, int exponent) {
  if (units == 0) return 0.0;
  uint magnitude = uint(abs(units));
  uint msb = uint(findMSB(magnitude));
  int e = int(msb) + exponent;
  uint bits = units < 0 ? 0x8000u : 0u;
  if (e >= -14) {
    uint sig = msb > 10u ? roundShiftRightEven(magnitude, msb - 10u) : magnitude << (10u - msb);
    if (sig >= 2048u) { sig = 1024u; ++e; }
    bits |= e >= 16 ? 0x7c00u : (uint(e + 15) << 10u) | (sig - 1024u);
  } else {
    int shift = exponent + 24;
    uint mantissa = shift >= 0 ? magnitude << uint(shift) : roundShiftRightEven(magnitude, uint(-shift));
    bits |= min(mantissa, 1024u);
  }
  return float(f16FromBits(bits));
}
float nrFdpa8(float a[8], float b[8], float acc) {
  if ((floatBitsToUint(acc) & 0x7f800000u) == 0x7f800000u) return acc;
  int e = acc != 0.0 ? nrExponent(acc, -14) : -21;
  for (uint i = 0u; i < 8u; ++i)
    if (a[i] != 0.0 && b[i] != 0.0) e = max(e, nrExponent(a[i], -14) + nrExponent(b[i], -14));
  float scale = nrPow2(24 - e);
  int units = int(trunc(acc * scale));
  for (uint i = 0u; i < 8u; ++i) units += int(trunc((a[i] * b[i]) * scale));
  return nrFixedHalf(units, e - 24);
}
float nrWindowExp(float score) {
  float x = clamp(roundF16(fma(score, 0.044921875, 1.30078125)), 1.03125, 1.5693359375);
  return float(f16FromBits(((f16Bits(float16_t(x)) << 5u) + 0x8000u) & 0xffffu));
}
float nrVitExp(float score) {
  float x = clamp(roundF16(fma(score, 0.08953857421875, 1.708984375)), 1.439453125, 1.9775390625);
  return float(f16FromBits(((f16Bits(float16_t(x)) << 4u) + 0x4000u) & 0xffffu));
}
#endif
