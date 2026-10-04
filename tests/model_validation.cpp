// Local model validation. Exported fixtures are produced by the selected
// Vulkan backend, never by NVIDIA's runtime. The direct WGSL diagnostic can
// consume them to independently check the software publication schedule.
#include "json.h"
#include "kernels.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"
#include "sha256.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
std::string argument(int argc, char** argv, const char* name, const std::string& fallback = "") {
  for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], name)) return argv[i + 1];
  return fallback;
}
bool flag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], name)) return true;
  return false;
}
void require(bool condition, const std::string& description) {
  if (!condition) throw std::runtime_error(description);
}
Bytes read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  require(bool(file), "cannot read " + path.string());
  const auto length = file.tellg(); require(length >= 0, "cannot measure " + path.string());
  Bytes bytes(size_t(length), 0); file.seekg(0);
  if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  require(bool(file), "short read " + path.string()); return bytes;
}
void write(const std::filesystem::path& path, const void* data, size_t size) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  require(bool(file), "cannot write " + path.string());
  file.write(reinterpret_cast<const char*>(data), size);
  require(bool(file), "short write " + path.string());
}
void write(const std::filesystem::path& path, const Bytes& data) { write(path, data.data(), data.size()); }
std::string quote(const std::string& value) {
  std::string result = "\"";
  for (char c : value) { if (c == '\\' || c == '\"') result += '\\'; result += c; }
  return result + '"';
}
uint32_t next(uint32_t& state) { state = state * 1664525u + 1013904223u; return state; }
// This regression input uses fixed-point colours and a 12-uniform noise sum.
// It is reproducible without vendor-specific log/sin/cos instructions, but it
// is not a captured game frame or the renderer's Box-Muller noise distribution.
std::vector<float> features(const nr::Geometry& geometry) {
  std::vector<float> result(size_t(geometry.fullWidth) * geometry.fullHeight * 16);
  for (uint32_t y = 0; y < geometry.fullHeight; ++y) for (uint32_t x = 0; x < geometry.fullWidth; ++x) {
    const uint32_t sx = x < geometry.validWidth ? x : std::min(geometry.validWidth - 1, 2 * geometry.validWidth - x - 2);
    const uint32_t sy = y < geometry.validHeight ? y : std::min(geometry.validHeight - 1, 2 * geometry.validHeight - y - 2);
    float* row = &result[(size_t(y) * geometry.fullWidth + x) * 16];
    uint32_t state = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ 0x91827364u;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      int sum = -1530; for (uint32_t j = 0; j < 12; ++j) sum += int(next(state) >> 24);
      row[channel] = num::roundF16(float(sum) * (1.0f / 256.0f));
      const uint32_t code = channel == 0 ? (sx * 3 + sy) & 255u : channel == 1 ? (sx + sy * 5 + 37) & 255u : ((sx / 16 ^ sy / 16) * 53 + 61) & 255u;
      const float centered = num::roundF16((float(code) * (1.0f / 256.0f) - .5f) * .125f);
      row[4 + channel] = row[7 + channel] = centered; // reset history = current proxy
    }
    row[3] = 1; row[10] = 0; row[11] = .25f;
    row[12] = 1; row[13] = row[14] = .75f; row[15] = 0;
  }
  return result;
}
struct Metrics {
  uint64_t samples = 0, differentBits = 0, signedZeros = 0, nonfinite = 0;
  double squared = 0, maxAbs = 0;
  void sample(float actual, float expected, bool equalBits) {
    ++samples;
    if (!equalBits) ++differentBits;
    if (!equalBits && actual == 0 && expected == 0) ++signedZeros;
    if (!std::isfinite(actual) || !std::isfinite(expected)) { ++nonfinite; return; }
    const double difference = double(actual) - expected;
    squared += difference * difference; maxAbs = std::max(maxAbs, std::fabs(difference));
  }
  double rms() const { return std::sqrt(squared / std::max<uint64_t>(1, samples - nonfinite)); }
  std::string json() const {
    std::ostringstream out; out << std::setprecision(12)
      << "{\"samples\":" << samples << ",\"differentBits\":" << differentBits
      << ",\"signedZeros\":" << signedZeros << ",\"nonfinite\":" << nonfinite
      << ",\"maxAbs\":" << maxAbs << ",\"rmse\":" << rms() << '}'; return out.str();
  }
  void print(const std::string& label) const {
    printf("%-22s values=%llu different=%llu signedZero=%llu maxAbs=%.9g rmse=%.9g nonfinite=%llu\n",
      label.c_str(), (unsigned long long)samples, (unsigned long long)differentBits,
      (unsigned long long)signedZeros, maxAbs, rms(), (unsigned long long)nonfinite);
  }
};
Metrics compareE4(const Bytes& actual, const Bytes& expected) {
  require(actual.size() == expected.size(), "E4 reference length differs"); Metrics result;
  auto decode = [](uint8_t value) { return (value & 0x7fu) == 0x7fu ? 0.0f : num::e4m3ToF32(value); };
  for (size_t i = 0; i < actual.size(); ++i) result.sample(decode(actual[i]), decode(expected[i]), actual[i] == expected[i]);
  return result;
}
Metrics compareF32(const Bytes& actual, const Bytes& expected) {
  require(actual.size() == expected.size() && actual.size() % 4 == 0, "F32 reference length differs"); Metrics result;
  for (size_t i = 0; i < actual.size(); i += 4) {
    float a, b; uint32_t aa, bb; memcpy(&a, actual.data() + i, 4); memcpy(&b, expected.data() + i, 4);
    memcpy(&aa, actual.data() + i, 4); memcpy(&bb, expected.data() + i, 4); result.sample(a, b, aa == bb);
  }
  return result;
}
Bytes compose(const Bytes& head, const std::vector<float>& input, const nr::Geometry& geometry) {
  require(head.size() == size_t(geometry.fullWidth) * geometry.fullHeight * 16, "head shape differs");
  std::vector<float> rgb(size_t(geometry.validWidth) * geometry.validHeight * 3);
  for (uint32_t y = 0; y < geometry.validHeight; ++y) for (uint32_t x = 0; x < geometry.validWidth; ++x)
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const size_t token = size_t(y) * geometry.fullWidth + x;
      float residual; memcpy(&residual, head.data() + (token * 4 + channel) * 4, 4);
      // Same centred proxy, no previous-frame feedback, style 0, intensity 1.
      // This metric is the published display-proxy RGB, before game HDR tone
      // transfer. It does not certify the game compositor or temporal quality.
      float value = std::fma(residual, .03125f, input[token * 16 + 4 + channel]) * 8.0f + .5f;
      value = std::clamp(value, 0.0f, 1.0f);
      rgb[(size_t(y) * geometry.validWidth + x) * 3 + channel] = num::f32FromBits(num::f32Bits(value) & 0xffffe000u);
    }
  Bytes result(rgb.size() * 4); memcpy(result.data(), rgb.data(), result.size()); return result;
}
struct Boundary { Bytes data; uint32_t width, height, channels; nr::Format format = nr::Format::E4; };
struct Run {
  Bytes head;
  std::map<std::string, Boundary> boundaries;
  std::vector<double> gpuMs;
  uint64_t activationBytes = 0;
  uint32_t allocations = 0, dispatches = 0;
};
Run execute(vk::Context& context, nr::Model& model, nr::Kernels& kernels,
            const nr::Geometry& geometry, const nr::Activation& input, bool capture, int frames, bool intermediates = false, bool headOnly = false) {
  nr::Graph graph(context, model, kernels, geometry, {.captureBoundaries = capture && !headOnly, .captureIntermediates = intermediates, .fusedBlocks = false});
  Run result; VkQueryPool timestamps = context.createTimestampPool(2);
  for (int frame = -1; frame < frames; ++frame) {
    context.resetDescriptorPool(); auto commands = context.beginCommands();
    vkCmdResetQueryPool(commands, timestamps, 0, 2);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
    graph.record(commands, input);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
    context.endAndSubmit(commands, true);
    auto time = context.readTimestampsMs(timestamps, 2);
    if (frame >= 0) result.gpuMs.push_back(time[1] - time[0]);
    auto head = context.download(graph.head().buffer, graph.head().validBytes());
    if (frame == -1) result.head = std::move(head);
    else require(head == result.head, "head changed when the identical command graph was recorded/submitted again");
    result.dispatches = kernels.dispatchCount();
    printf("modelcheck %s frame %d GPU %.3f ms, %u dispatches\n", capture ? "capture" : "production", frame, time[1] - time[0], result.dispatches);
  }
  vkDestroyQueryPool(context.device(), timestamps, nullptr);
  result.activationBytes = graph.activationBytes(); result.allocations = graph.activationCount();
  if (capture && !headOnly) for (const auto& name : nr::Graph::referenceBoundaryNames()) {
    const auto found = graph.boundaries().find(name); require(found != graph.boundaries().end(), "missing boundary " + name);
    const auto& activation = *found->second; require(activation.format == nr::Format::E4, "boundary is not E4: " + name);
    uint32_t width = 0, height = 0;
    if (activation.rows == geometry.fullWidth * geometry.fullHeight) { width = geometry.fullWidth; height = geometry.fullHeight; }
    else for (auto level : geometry.levels) if (activation.rows == level.width * level.height) { width = level.width; height = level.height; break; }
    require(width != 0, "unknown boundary geometry " + name);
    result.boundaries[name] = {context.download(activation.buffer, activation.validBytes()), width, height, activation.channels, activation.format};
  }
  if (intermediates && !headOnly) for (const auto& [name, activation] : graph.boundaries()) if (name.rfind("block-0/", 0) == 0)
    result.boundaries[name] = {context.download(activation->buffer, activation->validBytes()), geometry.fullWidth, geometry.fullHeight, activation->channels, activation->format};
  return result;
}
} // namespace

int runModelValidation(int argc, char** argv) {
  const std::filesystem::path modelPath = argument(argc, argv, "--model");
  const std::filesystem::path destination = argument(argc, argv, "--fixture");
  const std::filesystem::path referencePath = argument(argc, argv, "--reference");
  const std::string shaderDirectory = argument(argc, argv, "--shaders", (std::filesystem::path(argv[0]).parent_path() / "shaders").string());
  const int width = std::stoi(argument(argc, argv, "--width", "320"));
  const int height = std::stoi(argument(argc, argv, "--height", "320"));
  const int frames = std::stoi(argument(argc, argv, "--frames", "3"));
  if (modelPath.empty() || destination.empty()) {
    fprintf(stderr, "usage: dlss5vk modelcheck --model <dir> --fixture <export dir> [--reference <export dir>] [--width 320 --height 320 --frames 3] [--require-exact]\n"); return 2;
  }
  require(width >= 320 && height >= 320 && frames >= 1, "modelcheck needs width/height >= 320 and frames >= 1");
  require(referencePath.empty() || std::filesystem::absolute(destination).lexically_normal() != std::filesystem::absolute(referencePath).lexically_normal(), "export path must differ from reference path");
  vk::Context context;
  require(context.isAmd() || context.isReference(), "modelcheck is a native AMD/reference validation hook");
  const std::string backend = vk::backendName(context.backend());
  printf("modelcheck device: %s, backend: %s (no NVIDIA runtime parity)\n", context.deviceName().c_str(), backend.c_str());
  auto start = std::chrono::steady_clock::now(); nr::Model model(context, modelPath.string(), true);
  printf("verified real model loaded in %.3f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  nr::Kernels kernels(context, shaderDirectory); kernels.setSiluTable(ref::siluTable());
  const auto geometry = nr::Geometry::fromValid(uint32_t(width), uint32_t(height));
  const auto inputValues = features(geometry);
  const auto inputDigest = sha256Hex(reinterpret_cast<const uint8_t*>(inputValues.data()), inputValues.size() * 4);
  std::filesystem::create_directories(destination);
  write(destination / "features.f32", inputValues.data(), inputValues.size() * 4);
  const auto modelManifest = read(modelPath / "manifest.json");
  const auto modelDigest = sha256Hex(modelManifest.data(), modelManifest.size());
  json::Value referenceManifest;
  if (!referencePath.empty()) {
    const auto bytes = read(referencePath / "manifest.json"); referenceManifest = json::parse(std::string(bytes.begin(), bytes.end()));
    require(referenceManifest["sourceDimensions"][0].integer() == width && referenceManifest["sourceDimensions"][1].integer() == height, "reference valid dimensions differ");
    require(referenceManifest["fullDimensions"][0].integer() == geometry.fullWidth && referenceManifest["fullDimensions"][1].integer() == geometry.fullHeight, "reference full dimensions differ");
    const auto referenceFeatures = read(referencePath / referenceManifest["inputFeatures"]["file"].str());
    require(referenceFeatures.size() == inputValues.size() * 4 && !memcmp(referenceFeatures.data(), inputValues.data(), referenceFeatures.size()), "reference feature bytes differ");
    require(referenceManifest["modelManifestSha256"].str() == modelDigest, "reference used a different model manifest");
  }
  nr::Activation input;
  input.rows = geometry.fullWidth * geometry.fullHeight; input.allocRows = nr::alignRows(input.rows);
  input.channels = 16; input.format = nr::Format::F32;
  input.buffer = context.createBuffer(input.validBytes(), false, "modelcheck shared features");
  context.upload(input.buffer, inputValues.data(), inputValues.size() * 4);
  const Run production = execute(context, model, kernels, geometry, input, false, frames);
  const bool headOnly=flag(argc,argv,"--head-only");
  const Run captured = execute(context, model, kernels, geometry, input, true, 1, flag(argc, argv, "--intermediates"),headOnly);
  context.destroyBuffer(input.buffer);
  require(production.head == captured.head, "capture schedule changed the production head");
  const auto composed = compose(production.head, inputValues, geometry);
  const auto finiteHead = compareF32(production.head, production.head);
  require(finiteHead.nonfinite == 0, "model head contains nonfinite values");
  write(destination / "head.f32", production.head); write(destination / "captured-head.f32", captured.head);
  write(destination / "composed-rgb.f32", composed);
  std::ostringstream blocks, transitions, metricEntries; bool firstBlock = true, firstTransition = true, firstMetric = true;
  uint64_t different = 0; size_t comparedBoundaries = 0;
  auto metric = [&](const std::string& name, const Metrics& stats) {
    if (!firstMetric) metricEntries << ','; firstMetric = false;
    metricEntries << quote(name) << ':' << stats.json(); stats.print(name); different += stats.differentBits;
    require(stats.nonfinite == 0, "nonfinite comparison " + name);
  };
  for (const auto& name : nr::Graph::referenceBoundaryNames()) {
    if(headOnly)break;
    const auto& boundary = captured.boundaries.at(name); const std::string file = name + ".u8";
    write(destination / file, boundary.data);
    std::ostringstream entry;
    entry << "{\"" << (name.rfind("block-", 0) == 0 ? "block" : "id") << "\":";
    if (name.rfind("block-", 0) == 0) entry << name.substr(6); else entry << quote(name.substr(11));
    entry << ",\"width\":" << boundary.width << ",\"height\":" << boundary.height << ",\"channels\":" << boundary.channels << ",\"file\":" << quote(file) << '}';
    if (name.rfind("block-", 0) == 0) { if (!firstBlock) blocks << ','; firstBlock = false; blocks << entry.str(); }
    else { if (!firstTransition) transitions << ','; firstTransition = false; transitions << entry.str(); }
    if (!referencePath.empty()) { metric(name, compareE4(boundary.data, read(referencePath / file))); ++comparedBoundaries; }
  }
  for (const auto& [name, boundary] : captured.boundaries) if (name.rfind("block-0/", 0) == 0) {
    std::filesystem::create_directories(destination / "block-0");
    write(destination / (name + (boundary.format == nr::Format::F16 ? ".f16" : ".u8")), boundary.data);
  }
  if (!referencePath.empty()) {
    metric("head", compareF32(production.head, read(referencePath / "head.f32")));
    metric("composed reset RGB", compareF32(composed, read(referencePath / "composed-rgb.f32")));
  }
  const auto& policy=kernels.amdPolicy();
  std::ostringstream executedIdentity,executedSelection;
  executedIdentity << "{\"device_id\":" << quote(kernels.deviceId()) << ",\"driver_id\":" << quote(kernels.driverId())
    << ",\"model_sha256\":" << quote(model.manifestSha256()) << ",\"shader_sha256\":" << quote(kernels.shaderSha256())
    << ",\"baseline_shader_sha256\":" << quote(kernels.baselineShaderSha256()) << '}';
  executedSelection << "{\"kernels\":" << quote(kernels.selectedKernelMode()) << ",\"arithmetic\":" << quote(policy.arithmeticName())
    << ",\"tile_n\":" << policy.tileN << ",\"stage_k\":" << policy.stageK << ",\"window_queries\":" << policy.windowQueries
    << ",\"gemm\":" << quote(policy.gemmName())
    << ",\"window_layout\":" << quote(policy.windowLayoutName())
    << ",\"qkv_normalize\":" << quote(policy.qkvNormalizeName())
    << ",\"fusion\":" << (policy.fusion?"true":"false") << ",\"expert_fusion\":" << (policy.expertFusion?"true":"false")
    << ",\"ffn32_fusion\":" << (policy.ffn32Enabled()?"true":"false") << ",\"qkv32_fusion\":" << (policy.qkv32Enabled()?"true":"false")
    << ",\"block_fusion\":" << (policy.blockFusion?"true":"false") << ",\"hardware_publication\":" << (policy.hardwarePublication?"true":"false") << '}';
  std::ostringstream manifest;
  manifest << "{\"producer\":" << quote("OpenNR Vulkan " + backend + "; local validation, not NVIDIA capture")
    << ",\"captureImplementation\":" << quote(flag(argc, argv, "--intermediates") ? "decomposed" : "selected-production")
    << ",\"inputGenerator\":\"fixedpoint-proxy-clt-noise-v1\",\"inputFeaturesSha256\":" << quote(inputDigest)
    << ",\"modelManifestSha256\":" << quote(modelDigest)
    << ",\"identity\":" << executedIdentity.str() << ",\"selected\":" << executedSelection.str()
    << ",\"sourceDimensions\":[" << width << ',' << height << "],\"fullDimensions\":[" << geometry.fullWidth << ',' << geometry.fullHeight
    << "],\"inputFeatures\":{\"file\":\"features.f32\"},\"checks\":[\"boundaries\",\"head\"],\"blocks\":[" << blocks.str()
    << "],\"transitions\":[" << transitions.str() << "],\"referenceHead\":{\"file\":\"head.f32\"},"
    << "\"capturedHead\":{\"file\":\"captured-head.f32\",\"productionIdentical\":true},"
    << "\"compositionMetric\":{\"file\":\"composed-rgb.f32\",\"domain\":\"clamped truncated-half display-proxy RGB\",\"style\":0,\"intensity\":1,\"historyStrength\":0}}";
  const auto manifestText = manifest.str(); write(destination / "manifest.json", manifestText.data(), manifestText.size());
  auto times = production.gpuMs; std::sort(times.begin(), times.end());
  std::ostringstream report; report << std::setprecision(12)
    << "{\"format\":\"OpenNR-local-modelcheck-v1\",\"backend\":" << quote(backend)
    << ",\"captureImplementation\":" << quote(flag(argc, argv, "--intermediates") ? "decomposed" : "selected-production")
    << ",\"device\":" << quote(context.deviceName()) << ",\"nvidiaParityEstablished\":false,\"qualityThresholdApplied\":false,"
    << "\"identity\":" << executedIdentity.str() << ",\"selected\":" << executedSelection.str() << ','
    << "\"identicalFeaturesSha256\":" << quote(inputDigest) << ",\"modelManifestSha256\":" << quote(modelDigest)
    << ",\"productionRepeatable\":true,\"captureHeadIdentical\":true,\"nonfiniteHead\":0,\"dispatches\":" << production.dispatches
    << ",\"productionActivationBytes\":" << production.activationBytes << ",\"productionAllocations\":" << production.allocations
    << ",\"gpuMinMs\":" << times.front() << ",\"gpuMedianMs\":" << times[times.size() / 2] << ",\"gpuSamplesMs\": [";
  for (size_t i = 0; i < production.gpuMs.size(); ++i) { if (i) report << ','; report << production.gpuMs[i]; }
  report << "],\"comparedBoundaries\":" << comparedBoundaries << ",\"metrics\":{" << metricEntries.str() << "}}";
  const auto reportText = report.str(); write(destination / "modelcheck-report.json", reportText.data(), reportText.size());
  printf("modelcheck: %zu boundaries exported; production %.3f ms min / %.3f ms median, %.3f MiB / %u activations; feature SHA %s\n",
    captured.boundaries.size(), times.front(), times[times.size() / 2], production.activationBytes / 1048576.0, production.allocations, inputDigest.c_str());
  printf("MODEL CHECK EXECUTED: finite repeatable head, capture/production equal. This establishes no NVIDIA parity or visual-quality acceptance.\n");
  if (!referencePath.empty()) printf("REFERENCE COMPARISON: %s (%llu differing bytes/float bit patterns counted across reported comparisons)\n", different ? "DIFFERENT" : "BIT-EXACT", (unsigned long long)different);
  return flag(argc, argv, "--require-exact") && different ? 1 : 0;
}
