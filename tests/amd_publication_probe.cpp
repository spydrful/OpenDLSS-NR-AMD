// Standalone diagnostic for the isolated scalar-RTE half-publication shader.
// Does not select a production kernel or change the reference arithmetic.
#include "numeric.h"
#include "sha256.h"
#include "vk_context.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace {
std::vector<uint32_t> corpus() {
  std::vector<uint32_t> bits;
  // Every binary16 pattern, including both signed zeros and all NaN payloads.
  for (uint32_t half = 0; half != 65536; ++half)
    bits.push_back(num::f32Bits(num::f16ToF32(uint16_t(half))));
  // Every finite half rounding boundary, both signs, and the immediately
  // adjacent F32 values. Arithmetic is exact here (halves are F32-exact).
  for (uint32_t half = 0; half < 0x7bff; ++half) {
    const float lower = num::f16ToF32(uint16_t(half));
    const float upper = num::f16ToF32(uint16_t(half + 1));
    const uint32_t midpoint = num::f32Bits((lower + upper) * 0.5f);
    for (int delta = -1; delta <= 1; ++delta)
      for (uint32_t sign : {0u, 0x80000000u})
        bits.push_back(uint32_t(int64_t(midpoint) + delta) | sign);
  }
  // Finite overflow edge and neighboring F32 bit patterns. Publication is
  // IEEE half overflow to infinity; E4M3 saturation happens afterward.
  for (uint32_t sign : {0u, 0x80000000u})
    for (uint32_t magnitude : {0u, 1u, 0x007fffffu, 0x00800000u,
         0x33000000u, 0x33800000u, 0x387fffffu, 0x38800000u,
         0x477fefffu, 0x477ff000u, 0x477ff001u, 0x7f7fffffu,
         0x7f800000u, 0x7f800001u, 0x7fc00001u, 0x7fffffffu})
      bits.push_back(magnitude | sign);
  // Complete discarded-mantissa space for both retained-LSB parities in
  // every F32 exponent that can round to a nonzero finite half.
  for (uint32_t exponent = 102; exponent <= 142; ++exponent)
    for (uint32_t parity = 0; parity != 2; ++parity)
      for (uint32_t low = 0; low != 8192; ++low)
        for (uint32_t sign : {0u, 0x80000000u})
          bits.push_back(sign | (exponent << 23) | (parity << 13) | low);
  uint32_t state = 0x9070a3u;
  for (uint32_t i = 0; i < 262144; ++i) {
    state = state * 1664525u + 1013904223u;
    bits.push_back(state);
  }
  std::sort(bits.begin(), bits.end());
  bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
  return bits;
}

uint32_t expectedPublication(uint32_t bits) {
  if ((bits & 0x7fffffffu) >= 0x7f800000u) return bits;
  return num::f32Bits(num::roundF16(num::f32FromBits(bits)));
}

struct Counters {
  size_t candidateCpu = 0, baselineCpu = 0, candidateBaseline = 0;
  size_t rawHalfCpu = 0, rawHalfNan = 0, packedCpu = 0;
};
void report(const std::filesystem::path& path, const std::string& device,
            const VkPhysicalDeviceFloatControlsProperties& controls,
            const std::vector<uint32_t>& input, const Counters& count,
            bool dispatched, bool supported, bool validationEnabled) {
  const bool pass = dispatched && !count.candidateCpu && !count.baselineCpu &&
      !count.candidateBaseline && !count.rawHalfCpu && !count.rawHalfNan;
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot create probe report");
  out << "{\n  \"format\":\"OpenNR-half-publication-rte-probe-v1\",\n"
      << "  \"device\":\"" << device << "\",\n"
      << "  \"probeScope\":\"all half patterns and all finite half midpoints with F32 neighbors; complete discarded 13-bit mantissa/parity/exponent classes; seeded F32 edges\",\n"
      << "  \"exhaustiveF32\":false,\n  \"inputCount\":" << input.size()
      << ",\n  \"inputSha256\":\"" << sha256Hex(reinterpret_cast<const uint8_t*>(input.data()), input.size()*4) << "\",\n"
      << "  \"floatControls\":{\"roundingModeRTE16\":" << controls.shaderRoundingModeRTEFloat16
      << ",\"denormPreserve16\":" << controls.shaderDenormPreserveFloat16
      << ",\"signedZeroInfNanPreserve16\":" << controls.shaderSignedZeroInfNanPreserveFloat16
      << ",\"roundingIndependence\":" << controls.roundingModeIndependence
      << ",\"denormIndependence\":" << controls.denormBehaviorIndependence << "},\n"
      << "  \"requestedModesSupported\":" << (supported ? "true" : "false")
      << ",\n  \"gpuDispatched\":" << (dispatched ? "true" : "false")
      << ",\n  \"validationLayerEnabled\":" << (validationEnabled ? "true" : "false")
      << ",\n  \"mismatches\":{\"candidateVsCpu\":" << count.candidateCpu
      << ",\"baselineVsCpu\":" << count.baselineCpu << ",\"candidateVsBaseline\":" << count.candidateBaseline
      << ",\"rawHalfVsCpuFinite\":" << count.rawHalfCpu << ",\"rawHalfNanClass\":" << count.rawHalfNan
      << ",\"rejectedPackedVsCpu\":" << count.packedCpu << "},\n"
      << "  \"pass\":" << (pass ? "true" : "false")
      << ",\n  \"productionQualified\":false,\n  \"performanceClaim\":false\n}\n";
}
}

int main(int argc, char** argv) {
  try {
    if (argc != 4 || (std::string(argv[1]) != "info" && std::string(argv[1]) != "run"))
      throw std::runtime_error("usage: amd_publication_probe.exe info|run shader.spv report.json");
    vk::Context context(vk::Backend::AmdFast);
    VkPhysicalDeviceFloatControlsProperties controls{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &controls};
    vkGetPhysicalDeviceProperties2(context.physical(), &properties);
    // The probe requests only half modes. Reject devices coupling half and
    // F32; this probe does not also request or qualify F32 execution modes.
    const bool supported = controls.shaderRoundingModeRTEFloat16 && controls.shaderDenormPreserveFloat16 &&
        controls.shaderSignedZeroInfNanPreserveFloat16 &&
        controls.roundingModeIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE &&
        controls.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    auto input = corpus();
    Counters count;
    if (std::string(argv[1]) == "info" || !supported) {
      report(argv[3], context.deviceName(), controls, input, count, false, supported, context.validationEnabled());
      std::cout << "float-controls inquiry only; no GPU dispatch; supported=" << supported << "\n";
      return supported ? 0 : 2;
    }
    auto in = context.createBuffer(input.size()*4, false, "half-publication F32 input");
    auto out = context.createBuffer(input.size()*16, false, "half-publication output");
    context.upload(in, input.data(), in.size);
    auto module = context.loadShaderModule(argv[2]);
    auto pipeline = context.createComputePipeline(module, {}, "isolated half publication RTE", 32);
    const vk::Buffer* bindings[vk::kGenericBindings]{};
    bindings[0] = &in; bindings[1] = &out;
    auto descriptors = context.allocateSet(bindings);
    uint32_t size = uint32_t(input.size());
    auto commands = context.beginCommands();
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout(), 0, 1, &descriptors, 0, nullptr);
    vkCmdPushConstants(commands, context.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), &size);
    vkCmdDispatch(commands, (size+127)/128, 1, 1);
    context.endAndSubmit(commands);
    auto output = context.download(out, out.size);
    const auto* values = reinterpret_cast<const uint32_t*>(output.data());
    size_t printed = 0;
    for (size_t i = 0; i != input.size(); ++i) {
      const uint32_t expect = expectedPublication(input[i]);
      const uint32_t magnitude = input[i] & 0x7fffffffu;
      if (magnitude > 0x7f800000u)
        count.rawHalfNan += (values[4*i] & 0x7c00u) != 0x7c00u || (values[4*i] & 0x3ffu) == 0;
      else count.rawHalfCpu += values[4*i] != num::f16Bits(num::f32FromBits(input[i]));
      count.candidateCpu += values[4*i+1] != expect;
      count.baselineCpu += values[4*i+2] != expect;
      count.candidateBaseline += values[4*i+1] != values[4*i+2];
      count.packedCpu += values[4*i+3] != expect;
      if (values[4*i+1] != expect && printed++ < 16)
        printf("candidate mismatch input=%08x expect=%08x output=%08x baseline=%08x\n", input[i], expect, values[4*i+1], values[4*i+2]);
    }
    report(argv[3], context.deviceName(), controls, input, count, true, true, context.validationEnabled());
    context.destroyPipeline(pipeline);
    context.destroyBuffer(in); context.destroyBuffer(out);
    if (vk::Context::validationErrors()) throw std::runtime_error("Vulkan validation errors in probe");
    std::cout << input.size() << " inputs; candidate/CPU mismatches " << count.candidateCpu
        << "; candidate/baseline mismatches " << count.candidateBaseline << "; packed/CPU mismatches " << count.packedCpu << "\n";
    return count.candidateCpu || count.baselineCpu || count.candidateBaseline || count.rawHalfCpu || count.rawHalfNan ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n"; return 1;
  }
}
