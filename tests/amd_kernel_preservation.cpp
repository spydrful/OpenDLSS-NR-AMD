// Strict GPU-to-GPU preservation checks against an explicitly selected anchor.
// These are operator tests, not NVIDIA parity or a complete-model quality gate.
#include "kernels.h"
#include "numeric.h"
#include "reference.h"
#include "sha256.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

std::string argument(int argc, char** argv, const char* key, const std::string& fallback = {}) {
  for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == key) {
    if (i + 1 == argc) throw std::runtime_error(std::string("missing value for ") + key);
    return argv[i + 1];
  }
  return fallback;
}

struct Environment {
  std::vector<std::pair<std::string, std::optional<std::string>>> saved;
  void set(const char* key, const std::string& value) {
    const char* old = std::getenv(key);
    saved.emplace_back(key, old ? std::optional<std::string>(old) : std::nullopt);
    if (_putenv_s(key, value.c_str())) throw std::runtime_error(std::string("cannot set ") + key);
  }
  ~Environment() {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it)
      _putenv_s(it->first.c_str(), it->second ? it->second->c_str() : "");
  }
};

struct Runner {
  std::unique_ptr<vk::Context> context;
  std::unique_ptr<nr::Kernels> kernels;
  fs::path shaders;
  std::vector<uint8_t> fixtureBytes;
  Runner(const fs::path& directory, bool baseline, const std::string& anchor) : shaders(fs::absolute(directory)) {
    Environment environment;
    environment.set("DLSS5VK_AMD_KERNELS", baseline && anchor == "legacy" ? "baseline" : "optimized");
    if (baseline) {
      environment.set("DLSS5VK_AMD_ARITHMETIC", "k16");
      environment.set("DLSS5VK_AMD_GEMM", anchor == "rte32" ? "direct-rte" : anchor == "direct32" ? "direct" : "shared");
      environment.set("DLSS5VK_AMD_TILE_N", "16");
      environment.set("DLSS5VK_AMD_STAGE_K", "16");
      environment.set("DLSS5VK_AMD_WINDOW_QUERIES", anchor == "qualified32" || anchor == "direct32" || anchor == "rte32" ? "32" : "64");
      environment.set("DLSS5VK_AMD_WINDOW_LAYOUT", anchor == "rte32" ? "register-rte" : "staged");
      environment.set("DLSS5VK_AMD_FUSION", "0");
      environment.set("DLSS5VK_AMD_FFN32_FUSION", "0");
      environment.set("DLSS5VK_AMD_QKV32_FUSION", "0");
      environment.set("DLSS5VK_AMD_EXPERT_FUSION", "0");
      environment.set("DLSS5VK_AMD_BLOCK_FUSION", "0");
      environment.set("DLSS5VK_AMD_HARDWARE_PUBLICATION", "0");
      environment.set("DLSS5VK_AMD_TUNING", "");
    }
    context = std::make_unique<vk::Context>(vk::Backend::AmdFast);
    if (context->amdOptions().arithmetic != amd::Arithmetic::K16)
      throw std::runtime_error("amdcheck preservation requires k16; k32/final belong to quality experiments");
    kernels = std::make_unique<nr::Kernels>(*context, shaders.string());
    if (baseline) {
      const auto& actual = kernels->amdPolicy();
      const std::string expectedMode = anchor == "legacy" ? "baseline" : "optimized";
      const uint32_t expectedQueries = anchor == "qualified32" || anchor == "direct32" || anchor == "rte32" ? 32u : 64u;
      const auto expectedGemm = anchor == "rte32" ? amd::Gemm::DirectRte : anchor == "direct32" ? amd::Gemm::Direct : amd::Gemm::Shared;
      const auto expectedLayout = anchor == "rte32" ? amd::WindowLayout::RegisterRte : amd::WindowLayout::Staged;
      if (kernels->selectedKernelMode() != expectedMode || actual.arithmetic != amd::Arithmetic::K16 || actual.gemm != expectedGemm ||
          actual.windowLayout != expectedLayout ||
          actual.tileN != 16 || actual.stageK != 16 || actual.windowQueries != expectedQueries ||
          actual.ffn32Enabled() || actual.qkv32Enabled() || actual.expertFusion || actual.blockFusion || actual.hardwarePublication)
        throw std::runtime_error("amdcheck comparison anchor did not select its explicit preserving N16/K16/Q policy");
    }
    kernels->setSiluTable(ref::siluTable());
  }
};

struct Buffers {
  vk::Context& context;
  std::vector<vk::Buffer> owned;
  std::vector<uint8_t>* fixtureBytes = nullptr;
  ~Buffers() { for (auto& buffer : owned) context.destroyBuffer(buffer); }
  vk::Buffer make(size_t bytes, const void* data) {
    auto buffer = context.createBuffer(bytes, false, "AMD preservation fixture");
    owned.push_back(buffer);
    context.upload(buffer, data, bytes);
    if (fixtureBytes) {
      for (unsigned shift = 0; shift < 64; shift += 8) fixtureBytes->push_back(uint8_t(uint64_t(bytes) >> shift));
      const auto* first = static_cast<const uint8_t*>(data);
      fixtureBytes->insert(fixtureBytes->end(),first,first + bytes);
    }
    return buffer;
  }
  nr::Activation activation(uint32_t rows, uint32_t channels, nr::Format format, const void* data) {
    nr::Activation out;
    out.rows = rows; out.allocRows = nr::alignRows(rows); out.channels = channels; out.format = format;
    out.buffer = make(size_t(out.allocRows) * channels * nr::formatBytes(format), data);
    return out;
  }
  nr::Activation output(uint32_t rows, uint32_t channels, nr::Format format) {
    // Full allocations retain canaries, including row padding and base offsets.
    std::vector<uint8_t> sentinel(size_t(nr::alignRows(rows)) * channels * nr::formatBytes(format), 0xA5);
    return activation(rows, channels, format, sentinel.data());
  }
};

uint32_t word(uint32_t& state) { state = state * 1664525u + 1013904223u; return state; }
uint8_t finiteE4(uint32_t& state, size_t index, bool edges) {
  static constexpr uint8_t witnesses[] = {0x00,0x80,0x01,0x81,0x07,0x87,0x08,0x88,
      0x37,0xB7,0x38,0xB8,0x39,0xB9,0x48,0xC8,0x7E,0xFE};
  const uint32_t value = word(state);
  if (edges && index % 29 == 0) return witnesses[(index / 29) % std::size(witnesses)];
  return uint8_t((value % 65u) | ((value >> 17) & 0x80u));
}
uint16_t finiteHalf(uint32_t& state, size_t index) {
  static constexpr uint16_t witnesses[] = {0x0000,0x8000,0x0001,0x8001,0x03FF,0x83FF,
      0x0400,0x8400,0x1400,0x9400,0x1800,0x9800,0x1A00,0x9A00,0x3BFF,0x3C00,0x3C01};
  const uint32_t value = word(state);
  if (index % 23 == 0) return witnesses[(index / 23) % std::size(witnesses)];
  return num::f16Bits(float(int(value % 4097u) - 2048) / 2048.0f);
}

std::string quote(const std::string& value) {
  std::ostringstream out; out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << char(c);
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
    else out << char(c);
  }
  return out.str() + '"';
}
void write(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  if (!out) throw std::runtime_error("cannot write " + path.string());
}
std::string hash(const std::vector<uint8_t>& bytes) {
  auto value = sha256Hex(bytes.data(),bytes.size());
  for (char& c : value) if (c >= 'A' && c <= 'F') c += 'a' - 'A';
  return value;
}

struct Pair {
  std::string name, baseline, candidate, baselineHash, candidateHash;
  uint64_t bytes = 0, different = 0, first = 0;
};
struct Results {
  fs::path root;
  std::vector<Pair> pairs;
  uint32_t operators = 0, failures = 0, vitCasesExecuted = 0;
  uint32_t pairedCasesExecuted = 0, windowPaddingCasesExecuted = 0;
  uint64_t comparedBytes = 0;
  void compare(const std::string& name, const std::vector<uint8_t>& baseline,
               const std::vector<uint8_t>& candidate) {
    if (baseline.size() != candidate.size()) throw std::runtime_error("paired output size differs: " + name);
    Pair pair; pair.name = name; pair.bytes = baseline.size(); pair.first = pair.bytes;
    pair.baseline = "baseline/" + name + ".bin"; pair.candidate = "candidate/" + name + ".bin";
    pair.baselineHash = hash(baseline); pair.candidateHash = hash(candidate);
    for (size_t i = 0; i < baseline.size(); ++i) if (baseline[i] != candidate[i]) {
      ++pair.different; pair.first = std::min<uint64_t>(pair.first, i);
    }
    write(root / pair.baseline, baseline); write(root / pair.candidate, candidate);
    comparedBytes += pair.bytes;
    if (pair.different) {
      ++failures;
      fprintf(stderr, "amdcheck FAIL %s: %llu differing bytes, first=%llu baseline=%02x candidate=%02x\n",
          name.c_str(), (unsigned long long)pair.different, (unsigned long long)pair.first,
          baseline[size_t(pair.first)], candidate[size_t(pair.first)]);
    }
    pairs.push_back(std::move(pair));
  }
};

struct GemmCase {
  uint32_t K, N, rows, variant, seed;
  std::optional<uint32_t> partition, flags, batches;
};
constexpr const char* kVitCoverage = "vit-k1024-k4096-partitions-v1";
const std::array<GemmCase,14>& vitCases() {
  // The old Cartesian suite stops at K512 and partition128. These bounded
  // cases cover the actual ViT depths/partitions with small rows/columns,
  // output formats, seeded residuals, SiLU, broadcasts, tails and canaries.
  static const std::array<GemmCase,14> cases{{
      {1024,16,1,1,0x9070B100u,256,0,1},
      {1024,48,63,1,0x9070B101u,512,0,1},
      {1024,128,73,1,0x9070B102u,0,24,1},
      {1024,48,73,1,0x9070B103u,256,83,1},
      {1024,16,63,1,0x9070B104u,512,99,1},
      {1024,128,1,1,0x9070B105u,256,19,1},
      {1024,48,73,1,0x9070B106u,512,35,1},
      {1024,128,63,1,0x9070B107u,0,16,1},
      {4096,16,1,1,0x9070B108u,1024,83,1},
      {4096,48,63,1,0x9070B109u,1024,99,1},
      {4096,128,73,1,0x9070B10Au,1024,0,1},
      {4096,48,73,1,0x9070B10Bu,1024,24,1},
      {1024,48,63,1,0x9070B10Cu,256,152,2},
      {4096,16,73,1,0x9070B10Du,1024,163,2}}};
  return cases;
}
std::string vitName(const GemmCase& test) {
  return "gemm-vit-K" + std::to_string(test.K) + "-N" + std::to_string(test.N) +
      "-R" + std::to_string(test.rows) + "-F" + std::to_string(test.flags.value()) +
      "-P" + std::to_string(test.partition.value()) + "-B" + std::to_string(test.batches.value());
}
constexpr const char* kPairedCoverage = "paired-preload-k160-k544-batch8-v1";
const std::array<GemmCase,14>& pairedCases() {
  // K160 first enters the K32-pair preload loop. K544 first leaves the
  // small-K runtime-zero seed shortcut. Public FP8 shapes/partitions stay
  // multiples of 32; odd K16 tails and P16 partitions are unsupported.
  // B8 cases exercise the actual expert batch family with nonzero input,
  // weight and output bases. Full allocation comparison includes row/N
  // canaries, residual formats, activation, broadcasts and dual outputs.
  static const std::array<GemmCase,14> cases{{
      {160,16,1,1,0x9070C100u,0,0,1},
      {160,48,73,1,0x9070C101u,0,24,1},
      {160,48,63,1,0x9070C102u,32,83,2},
      {160,48,129,1,0x9070C103u,160,99,2},
      {160,48,73,1,0x9070C104u,32,35,3},
      {160,16,73,1,0x9070C105u,0,152,8},
      {192,48,63,1,0x9070C106u,96,0,2},
      {192,48,73,1,0x9070C107u,32,163,8},
      {256,64,73,1,0x9070C108u,0,16,8},
      {256,128,73,1,0x9070C109u,0,152,8},
      {256,48,129,1,0x9070C10Au,32,83,8},
      {512,48,73,1,0x9070C10Bu,0,24,8},
      {544,48,1,1,0x9070C10Cu,32,0,1},
      {544,48,73,1,0x9070C10Du,544,99,2}}};
  return cases;
}
std::string pairedName(const GemmCase& test) {
  return "gemm-paired-K" + std::to_string(test.K) + "-N" + std::to_string(test.N) +
      "-R" + std::to_string(test.rows) + "-F" + std::to_string(test.flags.value()) +
      "-P" + std::to_string(test.partition.value()) + "-B" + std::to_string(test.batches.value());
}
struct WindowCase { uint32_t width, height, heads, shiftX, shiftY; };
constexpr const char* kWindowPaddingCoverage = "window-thin-padding-v1";
const std::vector<WindowCase>& windowPaddingCases() {
  // Minimal/thin extents leave most query batches and physical keys padded.
  // Both Q16 and Q32 policies run this same set in independent amdcheck runs.
  static const std::vector<WindowCase> cases = [] {
    std::vector<WindowCase> out;
    for (auto dimensions : {std::array<uint32_t,2>{1,1}, std::array<uint32_t,2>{1,9},
                            std::array<uint32_t,2>{9,1}})
      for (uint32_t heads : {1u,2u,4u}) for (uint32_t sx : {0u,4u}) for (uint32_t sy : {0u,4u})
        out.push_back({dimensions[0],dimensions[1],heads,sx,sy});
    return out;
  }();
  return cases;
}
std::string windowName(const WindowCase& test) {
  return "window-" + std::to_string(test.width) + "x" + std::to_string(test.height) +
      "-H" + std::to_string(test.heads) + "-S" + std::to_string(test.shiftX) + "x" + std::to_string(test.shiftY);
}
struct Output { std::vector<uint8_t> primary, dual; };

Output gemm(Runner& runner, const GemmCase& test, bool overdispatch = false) {
  auto& context = *runner.context; auto& kernels = *runner.kernels;
  Buffers storage{context,{},&runner.fixtureBytes};
  const bool oldResidual = test.variant >= 2, oldHalf = test.variant == 2 || test.variant == 4;
  const bool oldDual = test.variant == 2 || test.variant == 5;
  const uint32_t flags = test.flags.value_or((oldResidual ? 1u : 0u) |
      (oldResidual && test.variant != 4 ? 2u : 0u) | (test.variant >= 4 ? 8u : 0u) |
      (test.variant != 1 && !oldDual ? 16u : 0u) | (oldDual ? 32u : 0u) |
      (oldResidual && !oldHalf ? 64u : 0u) | (test.variant == 3 || test.variant == 4 ? 128u : 0u));
  const bool broadcast = (flags & 128u) != 0u;
  const uint32_t batches = test.batches.value_or(test.variant < 2 ? 1u : (test.variant == 3 || test.variant == 5 ? 3u : 2u));
  const bool residual = (flags & 1u) != 0u, residualHalf = residual && (flags & 64u) == 0u;
  const bool dual = (flags & 32u) != 0u, quantize = (flags & 16u) != 0u;
  const bool scaled = (flags & 2u) != 0u, silu = (flags & 8u) != 0u;
  if ((flags & ~251u) || !batches || (dual && quantize) || (scaled && !residual) || ((flags & 64u) && !residual))
    throw std::runtime_error("invalid preservation GEMM flags/batches");
  const uint32_t inputBase = 16, outputBase = 16;
  const uint32_t inputStride = inputBase + test.K * (broadcast ? 1 : batches) + 16;
  const uint32_t outputStride = outputBase + batches * test.N + 16;
  const uint32_t padded = nr::alignRows(test.rows), matrixN = test.N + 32, weightBase = 16;
  const uint32_t partition = test.partition.value_or((test.variant == 2 || test.variant == 5) && test.K >= 64 ? std::min(128u, test.K) : 0);
  if (partition && (partition % 16 || test.K % partition))
    throw std::runtime_error("invalid preservation GEMM partition");
  uint32_t state = test.seed;
  std::vector<uint8_t> input(size_t(padded) * inputStride), weights(size_t(batches) * test.K * matrixN);
  for (size_t i = 0; i < input.size(); ++i) input[i] = finiteE4(state, i, test.variant & 1);
  for (size_t i = 0; i < weights.size(); ++i) weights[i] = finiteE4(state, i, test.variant & 1);
  std::vector<uint8_t> residual8(size_t(padded) * outputStride);
  std::vector<uint16_t> residual16(residual8.size()), scales(test.N + 8);
  for (size_t i = 0; i < residual8.size(); ++i) {
    residual8[i] = finiteE4(state, i, false); residual16[i] = finiteHalf(state, i);
  }
  for (size_t i = 0; i < scales.size(); ++i)
    scales[i] = num::f16Bits(i & 1 ? -0.3125f : 0.5625f);
  auto in = storage.activation(test.rows, inputStride, nr::Format::E4, input.data());
  auto out = storage.output(test.rows, outputStride, quantize ? nr::Format::E4 : nr::Format::F16);
  auto outDual = storage.output(test.rows, outputStride, nr::Format::E4);
  auto res = residualHalf ? storage.activation(test.rows, outputStride, nr::Format::F16, residual16.data())
                          : storage.activation(test.rows, outputStride, nr::Format::E4, residual8.data());
  auto w = storage.make(weights.size(), weights.data());
  nr::Tensor aux; aux.raw = storage.make(scales.size() * 2, scales.data());
  nr::GemmFp8Args args;
  args.input = &in; args.inputColumnBase = inputBase; args.weights = &w;
  args.Nmatrix = matrixN; args.weightColumnOffset = weightBase;
  args.output = &out; args.outputColumnOffset = outputBase; args.rows = test.rows; args.K = test.K; args.N = test.N;
  args.batches = batches; args.broadcastInput = broadcast; args.partition = partition;
  args.residual = residual ? &res : nullptr; args.scaleResidual = scaled;
  args.auxTensor = scaled ? &aux : nullptr; args.auxByteOffset = 8;
  args.silu = silu; args.quantize = quantize; args.dualOutput = dual ? &outDual : nullptr;
  context.resetDescriptorPool();
  auto commands = context.beginCommands();
  if (!overdispatch) kernels.gemmFp8(commands, args);
  else {
    // Exercise the direct shader's workgroup-uniform row guard with tiny
    // buffers, including Z groups whose row bases exceed four million.
    const auto& policy = kernels.amdPolicy();
    if (!policy.directOperands() || test.rows > 128)
      throw std::runtime_error("overdispatch fixture requires direct GEMM and at most 128 rows");
    vk::SpecConstants specs;
    specs.add(0,test.K);specs.add(2,flags);specs.add(3,partition);
    specs.add(10,policy.publicationInterval());specs.add(11,policy.tileN);
    specs.add(12,16);specs.add(13,policy.hardwarePublication ? 1u : 0u);
    struct Push {
      uint32_t rows,N,Nmatrix,weightColumnOffset,inputStride,inputColumnBase;
      uint32_t outputStride,outputColumnOffset,auxHalfOffset,batches,columnGroups,splitStride;
    } push{test.rows,test.N,matrixN,weightBase,inputStride,inputBase,
           outputStride,outputBase,4,batches,(test.N+policy.tileN-1)/policy.tileN,0};
    const vk::Buffer* bindings[vk::kGenericBindings] = {};
    bindings[0]=&in.buffer;bindings[1]=&w;
    bindings[quantize ? 5 : 2]=&out.buffer;
    if(dual)bindings[5]=&outDual.buffer;
    if(residual)bindings[residualHalf ? 3 : 6]=&res.buffer;
    if(scaled)bindings[4]=&aux.raw;
    const std::string moduleName=policy.gemmShaderName();
    // Borrow the exact selected bytes used by ordinary dispatch and its
    // aggregate identity. Reopening the path could execute a replacement
    // shader while the session still reports the originally cached module.
    const auto module=kernels.diagnosticNativeGemmFp8Module();
    if(std::string(kernels.selectedKernelMode())!="optimized" || module.sourceName!=moduleName)
      throw std::runtime_error("raw overdispatch module differs from selected optimized GEMM");
    const std::string pipelineLabel=moduleName+" overdispatch";
    auto pipeline=context.createComputePipeline(module.module,specs,pipelineLabel.c_str(),32);
    const auto descriptors=context.allocateSet(bindings);
    vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);
    vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_COMPUTE,context.pipelineLayout(),0,1,&descriptors,0,nullptr);
    vkCmdPushConstants(commands,context.pipelineLayout(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(commands,batches*push.columnGroups,3,2);
    context.computeBarrier(commands);
    context.endAndSubmit(commands,true);
    context.destroyPipeline(pipeline);
    // Kernels owns the borrowed shader for the session; destroy only the raw
    // pipeline here. The descriptor set remains owned by Context's pool.
  }
  if (!overdispatch) context.endAndSubmit(commands, true);
  Output result; result.primary = context.download(out.buffer, out.buffer.size);
  if (dual) result.dual = context.download(outDual.buffer, outDual.buffer.size);
  return result;
}

std::vector<uint8_t> window(Runner& runner, uint32_t width, uint32_t height, uint32_t heads,
                            uint32_t sx, uint32_t sy) {
  auto& context = *runner.context; Buffers storage{context,{},&runner.fixtureBytes};
  const uint32_t rows = width * height, channels = heads * 32;
  std::vector<uint8_t> input(size_t(nr::alignRows(rows)) * channels * 3);
  uint32_t state = 0x90700000u + width * 100 + height * 3 + heads;
  for (size_t i = 0; i < input.size(); ++i) input[i] = finiteE4(state, i, false);
  std::vector<uint16_t> priors(size_t(heads) * 4096);
  for (uint32_t h = 0; h < heads; ++h) for (uint32_t q = 0; q < 64; ++q) for (uint32_t k = 0; k < 64; ++k)
    priors[h * 4096 + q * 64 + k] = num::f16Bits(float(int((q * 17 + k * 19 + h * 23) % 61) - 30) / 128.0f);
  auto in = storage.activation(rows, channels * 3, nr::Format::E4, input.data());
  auto out = storage.output(rows, channels, nr::Format::E4);
  auto prior = storage.make(priors.size() * 2, priors.data());
  context.resetDescriptorPool(); auto commands = context.beginCommands();
  runner.kernels->windowAttend(commands, in, prior, out, width, height, heads, sx, sy);
  context.endAndSubmit(commands, true);
  return context.download(out.buffer, out.buffer.size);
}

std::vector<uint8_t> normalize(Runner& runner, uint32_t rows, uint32_t heads, bool global) {
  auto& context = *runner.context; Buffers storage{context,{},&runner.fixtureBytes};
  const uint32_t channels = heads * 32;
  std::vector<uint16_t> input(size_t(nr::alignRows(rows)) * channels * 3);
  uint32_t state = 0xABCD9070u + rows + heads;
  for (size_t i = 0; i < input.size(); ++i) input[i] = finiteHalf(state, i);
  std::fill(input.begin(), input.begin() + channels * 3, uint16_t(0));
  std::vector<float> scales(heads + 4);
  for (uint32_t h = 0; h < heads; ++h) scales[h + 4] = h & 1 ? -0.4375f : 0.8125f;
  auto in = storage.activation(rows, channels * 3, nr::Format::F16, input.data());
  auto out = storage.output(rows, channels * 3, nr::Format::E4);
  nr::Tensor aux; aux.raw = storage.make(scales.size() * 4, scales.data());
  context.resetDescriptorPool(); auto commands = context.beginCommands();
  if (global) runner.kernels->globalNormalize(commands, in, aux, 16, out, rows, heads);
  else runner.kernels->windowNormalize(commands, in, aux, 16, out, rows, heads);
  context.endAndSubmit(commands, true);
  return context.download(out.buffer, out.buffer.size);
}

std::vector<uint8_t> publication(Runner& runner) {
  auto& context = *runner.context; Buffers storage{context,{},&runner.fixtureBytes};
  std::vector<uint16_t> values(65536);
  for (uint32_t i = 0; i < values.size(); ++i) values[i] = uint16_t(i);
  auto in = storage.activation(8192, 8, nr::Format::F16, values.data());
  auto out = storage.output(8192, 8, nr::Format::E4);
  context.resetDescriptorPool(); auto commands = context.beginCommands();
  runner.kernels->quantize(commands, in, out); context.endAndSubmit(commands, true);
  return context.download(out.buffer, out.buffer.size);
}

Output halfBoundary(Runner& runner, bool head) {
  auto& context = *runner.context; Buffers storage{context,{},&runner.fixtureBytes};
  constexpr uint32_t rows = 73;
  const uint32_t K = head ? 32 : 16, N = head ? 4 : 32, paddedN = head ? 16 : 32;
  std::vector<uint16_t> input(size_t(nr::alignRows(rows)) * K), weights(size_t(K) * paddedN);
  uint32_t state = 0xFF249070u;
  for (size_t i = 0; i < input.size(); ++i) input[i] = finiteHalf(state, i);
  for (size_t i = 0; i < weights.size(); ++i) weights[i] = finiteHalf(state, i);
  auto in = storage.activation(rows, K, nr::Format::F16, input.data());
  auto out = storage.output(rows, N, head ? nr::Format::F32 : nr::Format::F16);
  auto dual = storage.output(rows, N, nr::Format::E4);
  auto w = storage.make(weights.size() * 2, weights.data());
  nr::GemmF16Args args;
  args.input = &in; args.weights = &w; args.paddedN = paddedN; args.output = &out;
  args.dualOutput = head ? nullptr : &dual; args.rows = rows; args.K = K; args.N = N;
  context.resetDescriptorPool(); auto commands = context.beginCommands();
  runner.kernels->gemmF16(commands, args); context.endAndSubmit(commands, true);
  Output result; result.primary = context.download(out.buffer, out.buffer.size);
  if (!head) result.dual = context.download(dual.buffer, dual.buffer.size);
  return result;
}

std::string identity(const Runner& runner) {
  const auto& options = runner.kernels->amdPolicy();
  std::ostringstream out;
  out << "{\"device_id\":" << quote(runner.kernels->deviceId()) << ",\"driver_id\":" << quote(runner.kernels->driverId())
      << ",\"shader_sha256\":" << quote(runner.kernels->shaderSha256())
      << ",\"baseline_shader_sha256\":" << quote(runner.kernels->baselineShaderSha256())
      << ",\"arithmetic\":" << quote(options.arithmeticName())
      << ",\"kernels\":" << quote(runner.kernels->selectedKernelMode()) << ",\"tile_n\":" << options.tileN
      << ",\"stage_k\":" << options.stageK << ",\"window_queries\":" << options.windowQueries << ",\"fusion\":" << (options.fusion ? "true" : "false")
      << ",\"ffn32_fusion\":" << (options.ffn32Enabled() ? "true" : "false") << ",\"qkv32_fusion\":" << (options.qkv32Enabled() ? "true" : "false")
      << ",\"gemm\":\"" << options.gemmName() << '"'
      << ",\"window_layout\":\"" << options.windowLayoutName() << '"'
      << ",\"expert_fusion\":" << (options.expertFusion ? "true" : "false")
      << ",\"block_fusion\":" << (options.blockFusion ? "true" : "false")
      << ",\"hardware_publication\":" << (options.hardwarePublication ? "true" : "false")
      << ",\"validationEnabled\":" << (runner.context->validationEnabled() ? "true" : "false") << "}";
  return out.str();
}

void manifest(const Results& results, const Runner& baseline, const Runner& candidate,
              const fs::path& destination, uint32_t validationErrors, const std::string& anchor) {
  fs::create_directories(destination.parent_path());
  std::ofstream out(destination, std::ios::trunc);
  out << "{\n\"format\":\"OpenNR-amd-exact-manifest-v1\",\"suite\":\"operators\","
      << "\"comparison_anchor\":" << quote(anchor) << ","
      << "\"passed\":" << (results.failures == 0 && validationErrors == 0 ? "true" : "false")
      << ",\"operators\":" << results.operators << ",\"checks\":" << results.pairs.size()
      << ",\"mismatchedChecks\":" << results.failures << ",\"comparedBytes\":" << results.comparedBytes
      << ",\"validationErrors\":" << validationErrors << ",\"fixtureRoot\":" << quote(results.root.generic_string())
      << ",\"model_free\":true,\"fixture_sha256\":" << quote(hash(baseline.fixtureBytes))
      << ",\"fixture_hash_scope\":\"Ordered length-prefixed host uploads, including output canaries\""
      << ",\"identity\":" << identity(candidate) << ",\"baseline_identity\":" << identity(baseline) << ",\n";
  if (candidate.kernels->amdPolicy().directOperands()) {
    const auto& policy=candidate.kernels->amdPolicy();
    out << "\"raw_gemm_overdispatch\":{\"variant\":" << quote(policy.gemmShaderName())
        << ",\"tile_n\":" << policy.tileN << ",\"stage_k\":16,\"publication_interval\":" << policy.publicationInterval()
        << ",\"required_subgroup_size\":32,\"dispatch_count\":3},\n";
  }
  if (results.vitCasesExecuted != vitCases().size()) throw std::runtime_error("extended ViT suite did not finish");
  out << "\"extended_vit\":{\"coverage_marker\":" << quote(kVitCoverage) << ",\"case_count\":" << results.vitCasesExecuted << ",\"cases\":[";
  for (size_t i = 0; i < vitCases().size(); ++i) {
    const auto& test=vitCases()[i]; if (i) out << ',';
    out << "{\"name\":" << quote(vitName(test)) << ",\"K\":" << test.K << ",\"N\":" << test.N << ",\"rows\":" << test.rows
        << ",\"flags\":" << test.flags.value() << ",\"partition\":" << test.partition.value() << ",\"batches\":" << test.batches.value() << '}';
  }
  if (results.pairedCasesExecuted != pairedCases().size()) throw std::runtime_error("extended paired preload suite did not finish");
  out << "]},\n\"extended_paired\":{\"coverage_marker\":" << quote(kPairedCoverage) << ",\"case_count\":" << results.pairedCasesExecuted << ",\"cases\":[";
  for (size_t i = 0; i < pairedCases().size(); ++i) {
    const auto& test=pairedCases()[i]; if (i) out << ',';
    out << "{\"name\":" << quote(pairedName(test)) << ",\"K\":" << test.K << ",\"N\":" << test.N << ",\"rows\":" << test.rows
        << ",\"flags\":" << test.flags.value() << ",\"partition\":" << test.partition.value() << ",\"batches\":" << test.batches.value() << '}';
  }
  if (results.windowPaddingCasesExecuted != windowPaddingCases().size()) throw std::runtime_error("extended window padding suite did not finish");
  out << "]},\n\"extended_window_padding\":{\"coverage_marker\":" << quote(kWindowPaddingCoverage)
      << ",\"case_count\":" << results.windowPaddingCasesExecuted << ",\"cases\":[";
  for (size_t i = 0; i < windowPaddingCases().size(); ++i) {
    const auto& test=windowPaddingCases()[i]; if (i) out << ',';
    out << "{\"name\":" << quote(windowName(test)) << ",\"width\":" << test.width << ",\"height\":" << test.height
        << ",\"heads\":" << test.heads << ",\"shiftX\":" << test.shiftX << ",\"shiftY\":" << test.shiftY << '}';
  }
  out << "]},\n\"coverage\":[\"tails\",\"shifted-windows\",\"channel-families\",\"broadcasts\",\"split-k\","
         "\"residuals\",\"activation\",\"padding\",\"conversion-edge-cases\"," << quote(kVitCoverage) << ','
      << quote(kPairedCoverage) << ',' << quote(kWindowPaddingCoverage) << "],\n\"pairs\":[\n";
  for (size_t i = 0; i < results.pairs.size(); ++i) {
    const auto& pair = results.pairs[i];
    if (i) out << ",\n";
    out << "{\"name\":" << quote(pair.name) << ",\"baseline\":" << quote(pair.baseline)
        << ",\"candidate\":" << quote(pair.candidate) << ",\"bytes\":" << pair.bytes
        << ",\"differentBytes\":" << pair.different << ",\"baselineSha256\":" << quote(pair.baselineHash)
        << ",\"candidateSha256\":" << quote(pair.candidateHash) << "}";
  }
  out << "\n]}\n";
  if (!out) throw std::runtime_error("cannot write " + destination.string());
}
} // namespace

int runAmdKernelPreservation(int argc, char** argv) {
  try {
    const auto baselineDirectory = argument(argc, argv, "--baseline-shaders");
    const auto candidateDirectory = argument(argc, argv, "--shaders");
    const auto fixtureDirectory = argument(argc, argv, "--fixture");
    const auto anchor = argument(argc, argv, "--comparison-anchor", "legacy");
    if (anchor != "legacy" && anchor != "compact64" && anchor != "qualified32" && anchor != "direct32" && anchor != "rte32")
      throw std::runtime_error("--comparison-anchor must be legacy, compact64, qualified32, direct32 or rte32");
    if (baselineDirectory.empty() || candidateDirectory.empty() || fixtureDirectory.empty()) {
      fprintf(stderr, "usage: dlss5vk amdcheck --baseline-shaders <frozen dir> --shaders <candidate dir> --fixture <output dir> [--json <manifest>] [--comparison-anchor legacy|compact64|qualified32|direct32|rte32]\n");
      return 2;
    }
    if (fs::equivalent(fs::path(baselineDirectory), fs::path(candidateDirectory)))
      throw std::runtime_error("baseline/candidate shader directories must be distinct");
    const auto root = fs::absolute(fixtureDirectory);
    const auto jsonPath = fs::absolute(argument(argc, argv, "--json", (root / "manifest.json").string()));
    // A failed rerun cannot leave an earlier completed manifest looking current.
    fs::create_directories(jsonPath.parent_path());
    {
      std::ofstream incomplete(jsonPath, std::ios::trunc);
      incomplete << "{\"format\":\"OpenNR-amd-exact-manifest-v1\",\"suite\":\"operators\","
                 << "\"comparison_anchor\":" << quote(anchor) << ","
                    "\"passed\":false,\"status\":\"incomplete\",\"coverage\":[],\"pairs\":[]}\n";
      if (!incomplete) throw std::runtime_error("cannot initialize " + jsonPath.string());
    }
    // Check the user-selected policy before overriding only the route selection.
    const char* policy = std::getenv("DLSS5VK_AMD_ARITHMETIC");
    if (policy && *policy && std::string(policy) != "k16")
      throw std::runtime_error("amdcheck rejects non-k16 arithmetic");
    const uint32_t beforeValidation = vk::Context::validationErrors();
    Runner baseline(baselineDirectory, true, anchor), candidate(candidateDirectory, false, anchor);
    if (baseline.context->capabilities().properties.deviceID != candidate.context->capabilities().properties.deviceID ||
        baseline.context->deviceName() != candidate.context->deviceName())
      throw std::runtime_error("baseline/candidate contexts selected different GPUs");
    Results results; results.root = root;
    const uint32_t rows[] = {1,63,64,73,129}, depths[] = {32,64,128,256,512}, columns[] = {16,32,48,64};
    for (uint32_t K : depths) for (uint32_t N : columns) for (uint32_t row : rows) for (uint32_t variant = 0; variant < 6; ++variant) {
      GemmCase test{K,N,row,variant,0x90702400u + K * 17 + N * 31 + row * 43 + variant * 101};
      auto expected = gemm(baseline, test), actual = gemm(candidate, test);
      const auto name = "gemm-K" + std::to_string(K) + "-N" + std::to_string(N) + "-R" + std::to_string(row) + "-v" + std::to_string(variant);
      results.compare(name, expected.primary, actual.primary);
      if (!expected.dual.empty()) results.compare(name + "-dual", expected.dual, actual.dual);
      ++results.operators;
      if (results.operators % 50 == 0) printf("amdcheck progress: %u operators, %zu checks, %u mismatched\n", results.operators, results.pairs.size(), results.failures);
    }
    for (const auto& test : vitCases()) {
      const auto expected=gemm(baseline,test), actual=gemm(candidate,test);
      const auto name=vitName(test);
      results.compare(name,expected.primary,actual.primary);
      if (!expected.dual.empty()) results.compare(name+"-dual",expected.dual,actual.dual);
      ++results.operators; ++results.vitCasesExecuted;
    }
    for (const auto& test : pairedCases()) {
      const auto expected=gemm(baseline,test), actual=gemm(candidate,test);
      const auto name=pairedName(test);
      results.compare(name,expected.primary,actual.primary);
      if (!expected.dual.empty()) results.compare(name+"-dual",expected.dual,actual.dual);
      ++results.operators; ++results.pairedCasesExecuted;
    }
    if(candidate.kernels->amdPolicy().directOperands()) {
      for(uint32_t variant : {0u,3u,5u}) {
        GemmCase test{variant == 5 ? 512u : 32u,48,73,variant,0x90704e11u+variant};
        const auto expected=gemm(baseline,test),actual=gemm(candidate,test,true);
        const auto name="gemm-overdispatch-v"+std::to_string(variant);
        results.compare(name,expected.primary,actual.primary);
        if(!expected.dual.empty())results.compare(name+"-dual",expected.dual,actual.dual);
        ++results.operators;
      }
    }
    for (auto dimensions : {std::array<uint32_t,2>{8,8}, std::array<uint32_t,2>{11,13}})
      for (uint32_t heads : {1u,2u,4u}) for (uint32_t sx : {0u,4u}) for (uint32_t sy : {0u,4u}) {
        const auto name = "window-" + std::to_string(dimensions[0]) + "x" + std::to_string(dimensions[1]) + "-H" +
            std::to_string(heads) + "-S" + std::to_string(sx) + "x" + std::to_string(sy);
        auto expected = window(baseline,dimensions[0],dimensions[1],heads,sx,sy);
        auto actual = window(candidate,dimensions[0],dimensions[1],heads,sx,sy);
        results.compare(name,expected,actual); ++results.operators;
      }
    for (const auto& test : windowPaddingCases()) {
      auto expected=window(baseline,test.width,test.height,test.heads,test.shiftX,test.shiftY);
      auto actual=window(candidate,test.width,test.height,test.heads,test.shiftX,test.shiftY);
      results.compare(windowName(test),expected,actual);
      ++results.operators; ++results.windowPaddingCasesExecuted;
    }
    for (uint32_t row : rows) for (uint32_t heads : {1u,2u,4u}) for (bool global : {false,true}) {
      const auto name = std::string(global ? "global" : "window") + "-normalize-R" + std::to_string(row) + "-H" + std::to_string(heads);
      results.compare(name,normalize(baseline,row,heads,global),normalize(candidate,row,heads,global)); ++results.operators;
    }
    results.compare("publication-all-65536-half-patterns",publication(baseline),publication(candidate)); ++results.operators;
    for (bool head : {false,true}) {
      auto expected = halfBoundary(baseline,head), actual = halfBoundary(candidate,head);
      const std::string name = head ? "F24-head-f32-R73" : "F24-adapter-half-R73";
      results.compare(name,expected.primary,actual.primary);
      if (!head) results.compare(name + "-dual",expected.dual,actual.dual);
      ++results.operators;
    }
    const uint32_t validationErrors = vk::Context::validationErrors() - beforeValidation;
    if (baseline.fixtureBytes != candidate.fixtureBytes) throw std::runtime_error("baseline/candidate uploaded fixture inputs differ");
    manifest(results,baseline,candidate,jsonPath,validationErrors,anchor);
    printf("amdcheck %s: operators=%u checks=%zu differentChecks=%u bytes=%llu validationErrors=%u; %s\n",
        results.failures || validationErrors ? "FAIL" : "PASS", results.operators, results.pairs.size(), results.failures,
        (unsigned long long)results.comparedBytes, validationErrors, jsonPath.string().c_str());
    return results.failures || validationErrors ? 1 : 0;
  } catch (const std::exception& error) {
    fprintf(stderr,"amdcheck FAILED: %s\n",error.what()); return 1;
  }
}
