// CPU/GPU diagnostic only. Does not select or qualify a production kernel.
// The CPU oracle uses explicit std::fma to match common.glsl, rather than
// assuming the compiler fuses the plain expressions in reference.cpp.
#include "numeric.h"
#include "sha256.h"
#include "vk_context.h"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <vector>

namespace {
constexpr size_t kHalfPatterns = 65536;
constexpr size_t kStages = 5;
constexpr size_t kGpuWords = 16;
using Stages = std::array<uint32_t, kStages>;
float publication(float value) {
  if ((num::f32Bits(value) & 0x7fffffffu) >= 0x7f800000u) return value;
  return num::roundF16(value);
}
Stages oracle(uint32_t bits) {
  const float value = num::f32FromBits(bits);
  const float bounded = publication(std::min(std::max(value, -4.0f), 4.0f));
  const float absolute = publication(std::fabs(bounded));
  const float inner = publication(std::fma(-0.055908203125f, absolute, 0.447265625f));
  const float polynomial = publication(std::fma(bounded, inner, 0.89453125f));
  const float result = publication(value * polynomial);
  return {num::f32Bits(bounded), num::f32Bits(absolute), num::f32Bits(inner),
          num::f32Bits(polynomial), num::f32Bits(result)};
}
bool finiteInput(uint32_t bits) { return (bits & 0x7fffffffu) < 0x7f800000u; }
bool nanBits(uint32_t bits) { return (bits & 0x7fffffffu) > 0x7f800000u; }
std::string quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') { out += '\\'; out += char(c); }
    else if (c < 32) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", c); out += escaped; }
    else out += char(c);
  }
  return out + '"';
}
std::vector<uint32_t> corpus() {
  std::vector<uint32_t> bits;
  for (uint32_t half = 0; half < kHalfPatterns; ++half)
    bits.push_back(num::f32Bits(num::f16ToF32(uint16_t(half))));
  std::vector<uint32_t> extra;
  auto neighbors = [&](uint32_t magnitude, int radius = 1) {
    for (int delta = -radius; delta <= radius; ++delta)
      for (uint32_t sign : {0u, 0x80000000u})
        extra.push_back(uint32_t(int64_t(magnitude) + delta) | sign);
  };
  for (uint32_t half = 0; half < 0x7bff; ++half) {
    const float lo = num::f16ToF32(uint16_t(half));
    const float hi = num::f16ToF32(uint16_t(half + 1));
    const float midpoint = (lo + hi) * 0.5f;
    neighbors(num::f32Bits(midpoint));
    // Above +4 SiLU is linear with the exact half polynomial 1.7890625.
    // Inverting output half midpoints probes the final publication, in addition
    // to the bounded-input half midpoints used by the cubic branch.
    const float input = midpoint / 1.7890625f;
    if (input > 4.0f) neighbors(num::f32Bits(input), 2);
  }
  for (uint32_t magnitude : {1u, 0x007fffffu, 0x00800000u, 0x33000000u,
       0x33800000u, 0x38800000u, 0x40800000u, 0x477ff000u, 0x7f7fffffu})
    neighbors(magnitude);
  for (uint32_t sign : {0u, 0x80000000u})
    for (uint32_t magnitude : {0u, 0x7f800000u, 0x7f800001u, 0x7fc00001u, 0x7fffffffu})
      extra.push_back(sign | magnitude);
  uint32_t state = 0x9070a4u;
  for (uint32_t i = 0; i < 262144; ++i) {
    state = state * 1664525u + 1013904223u;
    extra.push_back(state);
  }
  std::sort(extra.begin(), extra.end());
  extra.erase(std::unique(extra.begin(), extra.end()), extra.end());
  const std::set<uint32_t> halfValues(bits.begin(), bits.end());
  for (uint32_t value : extra) if (!halfValues.count(value)) bits.push_back(value);
  return bits;
}
size_t cpuCases(const std::vector<uint32_t>& input) {
  size_t checks = 0;
  auto require = [&](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); ++checks; };
  require(std::fegetround() == FE_TONEAREST, "CPU oracle requires RNE rounding mode");
  for (const auto& pair : std::array<std::pair<uint32_t,uint32_t>, 7>{{
      {0u,0u}, {0x80000000u,0x80000000u}, {0xc0800000u,0x80000000u},
      {0xc0a00000u,0x80000000u}, {0x40800000u,0x40e50000u},
      {0x3f800000u,0x3fa4a000u}, {0xbf800000u,0xbf00c000u}}})
    require(oracle(pair.first)[4] == pair.second, "CPU SiLU fixed witness failed");
  require(oracle(0x7f800000u)[4] == 0x7f800000u, "CPU positive infinity rule failed");
  // True F32 FMA rounds this value to a half midpoint, then half RNE goes down.
  // A direct F16 FMA goes up and violates the prescribed double publication.
  require(oracle(0x3cc4c000u)[2] == 0x3ee44000u, "CPU F32-before-half FMA midpoint rule failed");
  require(nanBits(oracle(0xff800000u)[4]), "CPU negative infinity NaN-class rule failed");
  require(num::f32Bits(publication(num::f32FromBits(0x7f800001u))) == 0x7f800001u,
          "CPU publication changed signaling NaN bits");
  require(num::f32Bits(publication(num::f32FromBits(0xffc01234u))) == 0xffc01234u,
          "CPU publication changed signed quiet NaN payload");
  for (uint32_t half = 0; half < kHalfPatterns; ++half) {
    const uint32_t widened = num::f32Bits(num::f16ToF32(uint16_t(half)));
    require(input[half] == widened, "Half corpus order/coverage changed");
    if (finiteInput(widened)) {
      require(num::f16Bits(num::f32FromBits(widened)) == half, "CPU half roundtrip failed");
      const Stages values = oracle(widened);
      for (uint32_t value : values)
        require(num::f32Bits(publication(num::f32FromBits(value))) == value,
                "CPU SiLU checkpoint is not half-published");
    }
  }
  return checks;
}
void writeBytes(const std::filesystem::path& path, const void* bytes, size_t size) {
  if (std::filesystem::exists(path)) throw std::runtime_error("refusing existing diagnostic artifact");
  std::ofstream stream(path, std::ios::binary);
  stream.write(static_cast<const char*>(bytes), std::streamsize(size));
  if (!stream) throw std::runtime_error("cannot write diagnostic artifact");
}
std::string digest(const void* bytes, size_t size) {
  return sha256Hex(reinterpret_cast<const uint8_t*>(bytes), size);
}
struct Counters {
  std::array<size_t,kStages> candidateBaseline{}, candidateCpu{}, baselineCpu{};
  size_t baselineFunction = 0, candidateFunction = 0, directPublication = 0;
  size_t nonfinitePublicationBits = 0, e4CandidateBaseline = 0, e4Cpu = 0;
  size_t cpuFiniteInputs = 0, cpuNonfiniteInputsSkipped = 0;
  bool clean() const {
    return std::all_of(candidateBaseline.begin(),candidateBaseline.end(),[](size_t n){return !n;}) &&
        std::all_of(candidateCpu.begin(),candidateCpu.end(),[](size_t n){return !n;}) &&
        std::all_of(baselineCpu.begin(),baselineCpu.end(),[](size_t n){return !n;}) &&
        !baselineFunction && !candidateFunction && !directPublication &&
        !nonfinitePublicationBits && !e4CandidateBaseline && !e4Cpu;
  }
};
void array(std::ostream& out, const std::array<size_t,kStages>& values) {
  out << '['; for (size_t i=0;i<values.size();++i) { if(i)out<<',';out<<values[i]; } out << ']';
}
void report(const std::filesystem::path& file, const std::string& mode,
            const std::string& device, const std::string& driver,
            const VkPhysicalDeviceFloatControlsProperties& controls,
            bool supported, bool queried, bool dispatched, bool validation,
            size_t cpuChecks, const std::vector<uint32_t>& input,
            const std::vector<uint32_t>& expected, const std::vector<uint8_t>& output,
            const Counters& counts) {
  if (std::filesystem::exists(file)) throw std::runtime_error("refusing existing probe report");
  std::ofstream out(file);
  if (!out) throw std::runtime_error("cannot write probe report");
  out << "{\n  \"format\":\"OpenNR-silu-rte-probe-v1\",\n  \"mode\":" << quote(mode)
      << ",\n  \"device\":" << quote(device) << ",\n  \"driver\":" << quote(driver)
      << ",\n  \"halfPatterns\":65536,\n  \"additionalF32Witnesses\":" << input.size()-kHalfPatterns
      << ",\n  \"inputCount\":" << input.size() << ",\n  \"exhaustiveAllF32\":false,\n"
      << "  \"inputSha256\":" << quote(digest(input.data(),input.size()*4))
      << ",\n  \"cpuOracleSha256\":" << quote(digest(expected.data(),expected.size()*4))
      << ",\n  \"gpuOutputSha256\":" << (dispatched ? quote(digest(output.data(),output.size())) : "null")
      << ",\n  \"gpuOutputBytes\":" << output.size() << ",\n  \"cpuChecks\":" << cpuChecks
      << ",\n  \"cpuChecksPassed\":true,\n  \"deviceQueried\":" << (queried?"true":"false")
      << ",\n  \"gpuDispatched\":" << (dispatched?"true":"false")
      << ",\n  \"validationLayerEnabled\":" << (validation?"true":"false")
      << ",\n  \"requestedModesSupported\":" << (queried ? (supported?"true":"false") : "null")
      << ",\n  \"floatControls\":{\"roundingModeRTE16\":" << controls.shaderRoundingModeRTEFloat16
      << ",\"denormPreserve16\":" << controls.shaderDenormPreserveFloat16
      << ",\"signedZeroInfNanPreserve16\":" << controls.shaderSignedZeroInfNanPreserveFloat16
      << ",\"signedZeroInfNanPreserve32\":" << controls.shaderSignedZeroInfNanPreserveFloat32
      << ",\"roundingIndependence\":" << controls.roundingModeIndependence
      << ",\"denormIndependence\":" << controls.denormBehaviorIndependence << "},\n"
      << "  \"checkpointOrder\":[\"bounded\",\"absolute\",\"inner_fma\",\"polynomial_fma\",\"result_multiply\"],\n"
      << "  \"publicationPolicy\":[\"scalar-rte\",\"scalar-rte\",\"original-software\",\"scalar-rte\",\"original-software\"],\n"
      << "  \"mismatches\":{\"candidateVsBaselineByCheckpoint\":"; array(out, counts.candidateBaseline);
  out << ",\"candidateVsCpuByCheckpoint\":"; array(out, counts.candidateCpu);
  out << ",\"baselineVsCpuByCheckpoint\":"; array(out, counts.baselineCpu);
  out << ",\"baselineFunctionVsTrace\":" << counts.baselineFunction
      << ",\"candidateFunctionVsTrace\":" << counts.candidateFunction
      << ",\"directPublication\":" << counts.directPublication
      << ",\"nonfinitePublicationRawBits\":" << counts.nonfinitePublicationBits
      << ",\"e4CandidateVsBaseline\":" << counts.e4CandidateBaseline
      << ",\"e4VsCpu\":" << counts.e4Cpu << "},\n"
      << "  \"cpuFiniteInputsCompared\":" << counts.cpuFiniteInputs
      << ",\n  \"cpuNonfiniteInputsRawSiluSkipped\":" << counts.cpuNonfiniteInputsSkipped
      << ",\n  \"nonfiniteScope\":\"All half nonfinite inputs remain strict GPU old-vs-RTE raw-bit comparisons at every checkpoint and actual helper result. CPU SiLU raw NaN payloads after clamp/FMA/multiply are not an oracle; direct publication must preserve every nonfinite input F32 bit.\",\n"
      << "  \"cpuOracle\":\"Explicit std::fma and preserving software half publication; finite F32 input checkpoints and E4 outputs compared exactly.\",\n"
      << "  \"pass\":" << (dispatched && counts.clean()?"true":"false")
      << ",\n  \"productionQualified\":false,\n  \"performanceClaim\":false\n}\n";
  if (!out) throw std::runtime_error("incomplete probe report");
}
}

int main(int argc, char** argv) {
  try {
    if (argc != 4 || (std::string(argv[1])!="cpu" && std::string(argv[1])!="info" && std::string(argv[1])!="run"))
      throw std::runtime_error("usage: amd_silu_rte_probe.exe cpu|info|run shader.spv report.json");
    const std::string mode = argv[1];
    const std::filesystem::path file = argv[3], prefix=file.parent_path()/file.stem();
    if (!std::filesystem::is_directory(file.parent_path()) || std::filesystem::exists(file))
      throw std::runtime_error("report parent must exist and report must be new");
    auto input = corpus();
    const size_t cpuChecks = cpuCases(input);
    std::vector<uint32_t> expected; expected.reserve(input.size()*kStages);
    for (uint32_t value : input) { const auto stages=oracle(value); expected.insert(expected.end(),stages.begin(),stages.end()); }
    writeBytes(prefix.string()+".input-u32.bin",input.data(),input.size()*4);
    writeBytes(prefix.string()+".cpu-oracle-u32.bin",expected.data(),expected.size()*4);
    Counters counts;
    std::vector<uint8_t> output;
    VkPhysicalDeviceFloatControlsProperties controls{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    if (mode=="cpu") {
      report(file,mode,"","",controls,false,false,false,false,cpuChecks,input,expected,output,counts);
      std::cout << "CPU PASS " << cpuChecks << " checks; " << input.size() << " witnesses; no Vulkan device or GPU query\n";
      return 0;
    }
    vk::Context context(vk::Backend::AmdFast);
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &controls};
    vkGetPhysicalDeviceProperties2(context.physical(), &properties);
    const bool supported=controls.shaderRoundingModeRTEFloat16 && controls.shaderDenormPreserveFloat16 &&
        controls.shaderSignedZeroInfNanPreserveFloat16 &&
        controls.shaderSignedZeroInfNanPreserveFloat32 &&
        controls.roundingModeIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE &&
        controls.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    const auto& caps=context.capabilities();
    const std::string driver=caps.driverName+"|"+caps.driverInfo+"|"+std::to_string(caps.properties.driverVersion);
    // Reject unsupported controls before shader module or pipeline creation.
    if (mode=="info" || !supported) {
      report(file,mode,context.deviceName(),driver,controls,supported,true,false,context.validationEnabled(),cpuChecks,input,expected,output,counts);
      return supported?0:2;
    }
    if(input.size()>std::numeric_limits<uint32_t>::max())throw std::runtime_error("input corpus exceeds dispatch ABI");
    auto in=context.createBuffer(input.size()*4,false,"SiLU probe F32 input");
    auto out=context.createBuffer(input.size()*kGpuWords*4,false,"SiLU checkpoint raw output");
    context.upload(in,input.data(),in.size);
    auto module=context.loadShaderModule(argv[2]);
    auto pipeline=context.createComputePipeline(module,{},"isolated preserving SiLU RTE",32);
    const vk::Buffer* bindings[vk::kGenericBindings]{};bindings[0]=&in;bindings[1]=&out;
    auto descriptors=context.allocateSet(bindings);
    const uint32_t size=uint32_t(input.size());
    auto commands=context.beginCommands();
    vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);
    vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_COMPUTE,context.pipelineLayout(),0,1,&descriptors,0,nullptr);
    vkCmdPushConstants(commands,context.pipelineLayout(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(size),&size);
    vkCmdDispatch(commands,(size+127)/128,1,1);
    context.endAndSubmit(commands);
    output=context.download(out,out.size);
    writeBytes(prefix.string()+".gpu-u32.bin",output.data(),output.size());
    const auto* words=reinterpret_cast<const uint32_t*>(output.data());
    size_t printed=0;
    for(size_t i=0;i<input.size();++i) {
      const auto* row=words+i*kGpuWords;
      for(size_t stage=0;stage<kStages;++stage) {
        counts.candidateBaseline[stage]+=row[stage]!=row[stage+kStages];
        if(finiteInput(input[i])) {
          counts.baselineCpu[stage]+=row[stage]!=expected[i*kStages+stage];
          counts.candidateCpu[stage]+=row[stage+kStages]!=expected[i*kStages+stage];
        }
      }
      counts.baselineFunction+=row[10]!=row[4];counts.candidateFunction+=row[11]!=row[9];
      const uint32_t published=num::f32Bits(publication(num::f32FromBits(input[i])));
      counts.directPublication+=row[12]!=published || row[13]!=published;
      if(!finiteInput(input[i])) counts.nonfinitePublicationBits+=row[12]!=input[i] || row[13]!=input[i];
      counts.e4CandidateBaseline+=row[14]!=row[15];
      if(finiteInput(input[i])) {
        ++counts.cpuFiniteInputs;
        const auto code=num::e4m3FromF32(num::f32FromBits(expected[i*kStages+4]));
        counts.e4Cpu+=row[14]!=code || row[15]!=code;
      } else ++counts.cpuNonfiniteInputsSkipped;
      if(row[10]!=row[11] && printed++<16)
        printf("SiLU mismatch index=%zu input=%08x software=%08x rte=%08x\n",i,input[i],row[10],row[11]);
    }
    report(file,mode,context.deviceName(),driver,controls,true,true,true,context.validationEnabled(),cpuChecks,input,expected,output,counts);
    context.destroyPipeline(pipeline);context.destroyBuffer(in);context.destroyBuffer(out);
    if(vk::Context::validationErrors())throw std::runtime_error("Vulkan validation errors in SiLU probe");
    std::cout << input.size() << " witnesses; " << (counts.clean()?"GPU STRICT PASS":"GPU STRICT FAIL") << "; no production or performance qualification\n";
    return counts.clean()?0:1;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n';return 1; }
}
