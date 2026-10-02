// Matched local scene diagnostic using the shipping game shaders and real model.
// Generated scenes are regression inputs, not game captures or NVIDIA outputs.
#include "kernels.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"
#include "sha256.h"
#include "shader_identity.h"
#include "json.h"
#include <cctype>
#include <algorithm>
#include <array>
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
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
using Pixel = std::array<float, 4>;
struct Params {
  uint32_t fullWidth, fullHeight, width, height, seed, historyValid, autoMask, style;
  float tone, structure, skin, paperWhite, intensity, blendScale, historyStrength, enabled, colorStrength, maxRatio;
};
static_assert(sizeof(Params) == 72);
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string quoteText(const std::string& text) { std::ostringstream out;out<<'"';for(unsigned char c:text){if(c=='"'||c=='\\')out<<'\\'<<char(c);else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;else out<<char(c);}return out.str()+'"'; }
std::string argument(int argc, char** argv, const char* name, const std::string& fallback = "") {
  for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], name)) return argv[i + 1]; return fallback;
}
void write(const std::filesystem::path& path, const void* data, size_t length) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc); check(bool(file), "cannot write " + path.string());
  file.write(static_cast<const char*>(data), length); check(bool(file), "short write " + path.string());
}
void write(const std::filesystem::path& path, const Bytes& bytes) { write(path, bytes.data(), bytes.size()); }
Bytes read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate); check(bool(file), "cannot read " + path.string());
  const auto length = file.tellg(); check(length >= 0, "cannot measure " + path.string()); Bytes result(size_t(length), 0);
  file.seekg(0); file.read(reinterpret_cast<char*>(result.data()), result.size()); check(bool(file), "short read " + path.string()); return result;
}
std::string digest(const Bytes& bytes) { return sha256Hex(bytes.data(), bytes.size()); }
std::string captureIdentityHash(std::string hash) {for(char& c:hash)if(c>='a'&&c<='f')c-=('a'-'A');return hash;}
std::string captureShaderHash(const shader_identity::Source& source) {return captureIdentityHash(source.sha256);}
vk::Pipeline capturedShaderPipeline(vk::Context& context,const shader_identity::Source& source,const char* label) {
  // Keep verified bytes immutable through module creation, even if the source
  // file is replaced between provenance validation and this GPU recording.
  struct Module { VkDevice device;VkShaderModule handle=VK_NULL_HANDLE;
    ~Module(){if(handle)vkDestroyShaderModule(device,handle,nullptr);} } module{context.device()};
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize=source.words.size()*sizeof(uint32_t);info.pCode=source.words.data();
  VK_CHECK(vkCreateShaderModule(module.device,&info,nullptr,&module.handle));
  vk::SpecConstants noSpec;return context.createComputePipeline(module.handle,noSpec,label,0);
}
template<class T> std::string digest(const std::vector<T>& values) {
  return sha256Hex(reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(T));
}
void pfm(const std::filesystem::path& path, const Bytes& rgba, uint32_t width, uint32_t height) {
  check(rgba.size() == size_t(width) * height * 16, "PFM RGBA extent mismatch");
  std::ofstream file(path, std::ios::binary | std::ios::trunc); file << "PF\n" << width << ' ' << height << "\n-1.0\n";
  for (uint32_t y = height; y-- > 0;) for (uint32_t x = 0; x < width; ++x)
    file.write(reinterpret_cast<const char*>(rgba.data() + (size_t(y) * width + x) * 16), 12);
  check(bool(file), "cannot write PFM " + path.string());
}
struct Storage {
  vk::Context& context; std::vector<vk::Buffer> buffers;
  ~Storage() { for (auto& buffer : buffers) context.destroyBuffer(buffer); }
  vk::Buffer make(uint64_t bytes, const void* data = nullptr) {
    auto buffer = context.createBuffer(bytes, false, "matched scene diagnostic"); buffers.push_back(buffer);
    if (data) context.upload(buffer, data, bytes); else context.fillZero(buffer); return buffer;
  }
};
uint32_t next(uint32_t& state) { state = state * 1664525u + 1013904223u; return state; }
std::vector<Pixel> scene(uint32_t width, uint32_t height, bool hdr) {
  const size_t count = size_t(width) * height; std::vector<Pixel> source(count * 2 + 1);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const float u = float(x) / (width - 1), v = float(y) / (height - 1);
    const bool checker = ((x / 16) ^ (y / 16)) & 1;
    uint32_t state = x * 0x8da6b343u ^ y * 0xd8163841u ^ 0x91827364u;
    const float noise = float(int(next(state) >> 24) - 127) / 8192;
    Pixel color{std::max(.003f, .02f + .78f * u + noise), std::max(.003f, .02f + .78f * v - noise), checker ? .15f : .7f, .125f + float((x + y) & 7) / 8};
    if (hdr) {
      const float highlight = (x > width / 3 && x < width / 2 && y > height / 4 && y < height / 2) ? 16.0f : 1.0f;
      for (uint32_t c = 0; c < 3; ++c) color[c] *= highlight * (1 + 3 * u);
    }
    for (auto& value : color) value = num::roundF16(value); source[(size_t(y) * width + x) * 2] = color;
    // Fractional motion, jitter, edges and invalid reprojection are represented
    // in the same normalized float4 layout produced by the D3D12 pack shader.
    const float dx = checker ? 1.25f : -.75f, dy = checker ? -.5f : .25f;
    const bool valid = x + dx >= 0 && x + dx < width && y + dy >= 0 && y + dy < height;
    source[(size_t(y) * width + x) * 2 + 1] = {dx / width, dy / height, float(valid), 0};
  }
  source[count * 2] = {hdr ? 1.5f : .75f, 0, 0, 0}; return source;
}
std::vector<Pixel> history(uint32_t width, uint32_t height, bool poisoned) {
  std::vector<Pixel> result(size_t(width) * height);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    if (poisoned) result[size_t(y) * width + x] = {std::numeric_limits<float>::quiet_NaN(), -1234, 9999, 0};
    else result[size_t(y) * width + x] = {.1f + .7f * x / width, .2f + .6f * y / height, ((x / 16) ^ (y / 16)) & 1 ? .65f : .35f, 1};
  }
  return result;
}
void dispatch(vk::Context& context, vk::Pipeline pipeline, const vk::Buffer* const* bindings, const Params& params, bool pre) {
  auto commands = context.beginCommands(); auto set = context.allocateSet(bindings);
  vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
  vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout(), 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(commands, context.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
  vkCmdDispatch(commands, ((pre ? params.fullWidth : params.width) + 7) / 8, ((pre ? params.fullHeight : params.height) + 7) / 8, 1);
  context.endAndSubmit(commands, true);
}
bool sameDigest(std::string a, std::string b) {
  auto upper = [](unsigned char value) { return char(std::toupper(value)); };
  std::transform(a.begin(), a.end(), a.begin(), upper); std::transform(b.begin(), b.end(), b.begin(), upper); return a == b;
}
int recordedComposite(int argc, char** argv, const std::filesystem::path& capture, const std::filesystem::path& modelPath,
                      const std::filesystem::path& destination, const std::filesystem::path& referencePath) {
  const auto manifestBytes = read(capture / "manifest.json");
  const auto recorded = json::parse(std::string(manifestBytes.begin(), manifestBytes.end()));
  check(recorded["format"].str() == "OpenNR-game-capture-v1", "unsupported recorded-frame format");
  const bool gameCapture = recorded.has("gameCapture") && recorded["gameCapture"].boolean;
  const auto capturedWidth = recorded["width"].integer(), capturedHeight = recorded["height"].integer();
  check(capturedWidth > 0 && capturedHeight > 0 && capturedWidth <= 3840 && capturedHeight <= 2160, "recorded frame dimensions exceed runtime limits");
  const uint32_t width = uint32_t(capturedWidth), height = uint32_t(capturedHeight);
  const auto geometry = nr::Geometry::fromValid(width, height);
  check(geometry.fullWidth == recorded["fullWidth"].integer() && geometry.fullHeight == recorded["fullHeight"].integer(), "recorded model geometry differs");
  const auto executable = std::filesystem::path(argv[0]).parent_path();
  const std::string shaders = argument(argc, argv, "--shaders", (executable / "shaders").string());
  const std::filesystem::path gameShaders = argument(argc, argv, "--game-shaders", (executable / "game/shaders").string());
  const auto preSource=shader_identity::read((gameShaders/"game_preprocess.spv").string());
  const auto compositeSource=shader_identity::read((gameShaders/"game_composite.spv").string());
  const auto preHash=captureShaderHash(preSource),compositeHash=captureShaderHash(compositeSource);
  check(sameDigest(preHash, recorded["preprocessSpvSha256"].str()) && sameDigest(compositeHash, recorded["compositeSpvSha256"].str()), "recorded game shader hashes differ");
  const auto source = read(capture / "source-packed.f32"), capturedPrevious = read(capture / "previous-history.f32"), control = read(capture / "controls.bin"), expectedFeatures = read(capture / "features.f32");
  auto previous=capturedPrevious;
  const auto runtimeHead = read(capture / "head.f32"), runtimeOutput = read(capture / "scene-linear-rgba.f32");
  const size_t pixels = size_t(width) * height, fullPixels = size_t(geometry.fullWidth) * geometry.fullHeight;
  check(source.size() == (pixels * 2 + 1) * 16 && previous.size() == pixels * 16 && control.size() == sizeof(Params) && expectedFeatures.size() == fullPixels * 64 && runtimeHead.size() == fullPixels * 16 && runtimeOutput.size() == pixels * 16, "recorded binary file extent mismatch");
  check(recorded.has("filesSha256"), "recorded v1 capture is missing required binary hashes");
  const std::pair<const char*, const Bytes*> files[] = {{"source-packed.f32", &source}, {"previous-history.f32", &capturedPrevious}, {"controls.bin", &control}, {"features.f32", &expectedFeatures}, {"head.f32", &runtimeHead}, {"scene-linear-rgba.f32", &runtimeOutput}};
  for (const auto& [name, bytes] : files) {
    check(recorded["filesSha256"].has(name), std::string("recorded v1 capture is missing required hash: ") + name);
    check(sameDigest(digest(*bytes), recorded["filesSha256"][name].str()), std::string("recorded file hash differs: ") + name);
  }
  Params params; memcpy(&params, control.data(), sizeof(params));
  check(params.width == width && params.height == height && params.fullWidth == geometry.fullWidth && params.fullHeight == geometry.fullHeight, "recorded controls geometry differs");
  vk::Context context; check(context.isAmd() || context.isReference(), "recorded replay requires AMD/reference backend");
  nr::Model model(context, modelPath.string(), true);
  const auto modelHash=captureIdentityHash(model.manifestSha256());
  check(sameDigest(modelHash,recorded["modelManifestSha256"].str()),"recorded parsed model manifest hash differs");
  nr::Kernels kernels(context, shaders); kernels.setSiluTable(ref::siluTable());
  nr::Graph graph(context, model, kernels, geometry, {.fusedBlocks = false});
  const std::string historyMode=argument(argc,argv,"--history-mode","identical");
  check(historyMode=="identical"||historyMode=="evolved","--history-mode must be identical or evolved");
  const std::filesystem::path previousReplay=argument(argc,argv,"--previous-replay");
  check(historyMode=="evolved"||previousReplay.empty(),"--previous-replay requires evolved history mode");
  uint64_t replayHistoryFrame=0;
  if(historyMode=="evolved"){
    check(recorded.has("sequence_id"),"evolved replay requires bounded sequence metadata");
    if(previousReplay.empty()||!params.historyValid){previous.assign(pixels*16,0);params.historyValid=0;params.seed=0;}
    else{
      const auto bytes=read(previousReplay/"manifest.json");const auto prior=json::parse(std::string(bytes.begin(),bytes.end()));
      check(prior["format"].str()=="OpenNR-local-scene-composite-v1"&&prior["historyMode"].str()=="evolved","prior replay history is not an evolved diagnostic");
      check(prior["sequenceId"].str()==recorded["sequence_id"].str()&&prior["sourceFrameId"].integer()+1==recorded["frame_id"].integer(),"evolved replay history has a sequence/frame gap; restart from reset");
      check(prior["width"].integer()==width&&prior["height"].integer()==height&&prior["backend"].str()==vk::backendName(context.backend()),"prior replay geometry or backend differs");
      check(sameDigest(prior["modelManifestSha256"].str(),modelHash)&&sameDigest(prior["shaderSha256"].str(),kernels.shaderSha256()),"prior replay model or selected shader set differs");
      check(prior["arithmeticPolicy"].str()==kernels.amdPolicy().arithmeticName(),"prior replay arithmetic policy differs");
      const auto& selected=prior["selected"];const auto& policy=kernels.amdPolicy();
      check(selected["kernels"].str()==kernels.selectedKernelMode() && selected["arithmetic"].str()==(context.isReference()?"reference":policy.arithmeticName()),"prior replay kernel/arithmetic selection differs");
      check(selected["tile_n"].integer()==policy.tileN && selected["stage_k"].integer()==policy.stageK && selected["window_queries"].integer()==policy.windowQueries,"prior replay specialization differs");
      for(const auto& [name,value]:std::vector<std::pair<const char*,bool>>{{"fusion",policy.fusion},{"expert_fusion",policy.expertFusion},{"block_fusion",policy.blockFusion},{"hardware_publication",policy.hardwarePublication}})
        check(selected[name].kind==json::Value::Bool && selected[name].boolean==value,"prior replay fusion/publication selection differs");
      check(recorded.has("history_frame_ids")&&recorded["history_frame_ids"].size()==1&&recorded["history_frame_ids"][0].integer()==prior["sourceFrameId"].integer(),"recorded history ancestor differs from prior replay");
      previous=read(previousReplay/"recorded-published-history.f32");
      check(previous.size()==pixels*16&&sameDigest(digest(previous),prior["publishedHistorySha256"].str()),"prior replay history bytes/identity differ");
      const auto seed=prior["seed"].integer();check(seed>=0&&seed<UINT32_MAX,"prior replay seed exceeds range");params.seed=uint32_t(seed)+1;replayHistoryFrame=uint64_t(prior["sourceFrameId"].integer());
    }
  }
  Storage storage{context};
  nr::Activation features; features.rows = uint32_t(fullPixels); features.allocRows = nr::alignRows(features.rows); features.channels = 16; features.format = nr::Format::F32;
  features.buffer = storage.make(features.validBytes());
  auto input = storage.make(source.size(), source.data()), oldHistory = storage.make(previous.size(), previous.data()), output = storage.make(pixels * 16), nextHistory = storage.make(pixels * 16);
  auto pre=capturedShaderPipeline(context,preSource,"recorded game preprocess");
  auto composite=capturedShaderPipeline(context,compositeSource,"recorded game composite");
  const vk::Buffer* bindings[vk::kGenericBindings]{}; bindings[0] = &input; bindings[1] = &oldHistory; bindings[3] = &output; bindings[4] = &nextHistory; bindings[7] = &features.buffer;
  dispatch(context, pre, bindings, params, true);
  const auto computedFeatures = context.download(features.buffer, features.validBytes());
  if(historyMode=="identical")check(computedFeatures == expectedFeatures, "shipping preprocessing did not reproduce captured feature bits (input/history/controls or driver arithmetic differs)");
  std::map<std::string, Bytes> captures;
  for (uint32_t repeat = 0; repeat < 2; ++repeat) {
    context.resetDescriptorPool(); auto commands = context.beginCommands(); graph.record(commands, features); context.endAndSubmit(commands, true);
    bindings[2] = &graph.head().buffer; dispatch(context, composite, bindings, params, false);
    std::map<std::string, Bytes> result;
    result["features"] = computedFeatures; result["head"] = context.download(graph.head().buffer, graph.head().validBytes());
    result["scene-linear-rgba"] = context.download(output, pixels * 16); result["published-history"] = context.download(nextHistory, pixels * 16);
    if (!repeat) captures = std::move(result); else check(result == captures, "recorded replay changed with identical inputs/history/controls");
  }
  for (size_t pixel = 0; pixel < pixels; ++pixel) {
    Pixel value; memcpy(value.data(), captures["scene-linear-rgba"].data() + pixel * 16, 16);
    for (auto channel : value) check(std::isfinite(channel), "nonfinite recorded replay scene output");
    check(!memcmp(captures["scene-linear-rgba"].data() + pixel * 16 + 12, source.data() + pixel * 32 + 12, 4), "recorded replay changed alpha bits");
  }
  std::filesystem::create_directories(destination);
  for (const auto& [name, bytes] : captures) write(destination / ("recorded-" + name + ".f32"), bytes);
  write(destination / "recorded-source.f32", source); write(destination / "recorded-previous-history.f32", previous); write(destination / "recorded-controls.bin", &params,sizeof(params));
  write(destination / "recorded-runtime-head.f32", runtimeHead); write(destination / "recorded-runtime-scene-linear-rgba.f32", runtimeOutput);
  pfm(destination / "recorded-scene-linear-rgb.pfm", captures["scene-linear-rgba"], width, height); pfm(destination / "recorded-runtime-scene-linear-rgb.pfm", runtimeOutput, width, height);
  if (!referencePath.empty()) for (const auto* name : {"source.f32", "previous-history.f32", "controls.bin", "features.f32"}) {
    if(historyMode=="evolved"&&(!strcmp(name,"previous-history.f32")||!strcmp(name,"features.f32")))continue;
    const std::string file = std::string("recorded-") + name; check(read(destination / file) == read(referencePath / file), "recorded reference input differs: " + file);
  }
  std::ostringstream manifest;
  manifest << "{\"format\":\"OpenNR-local-scene-composite-v1\",\"backend\":\"" << vk::backendName(context.backend()) << "\",\"width\":" << width << ",\"height\":" << height
    << ",\"syntheticScene\":" << (gameCapture ? "false" : "true") << ",\"capturedGameFrames\":" << (gameCapture ? "true" : "false") << ",\"nvidiaParityEstablished\":false,\"visualQualityAccepted\":false,\"sourceCaptureManifestSha256\":\"" << digest(manifestBytes)
    << "\",\"sourceFrameId\":" << recorded["frame_id"].integer() << ",\"historyMode\":\"" << historyMode << "\",\"sequenceId\":\"" << (recorded.has("sequence_id")?recorded["sequence_id"].str():"legacy-single-frame")
    << "\",\"seed\":" << params.seed << ",\"reset\":" << (params.historyValid?"false":"true") << ",\"historyFrameIds\":[" << (params.historyValid?(historyMode=="evolved"?std::to_string(replayHistoryFrame):(recorded.has("history_frame_ids")&&recorded["history_frame_ids"].size()?std::to_string(recorded["history_frame_ids"][0].integer()):"")):"")
    << "],\"historyValid\":" << params.historyValid << ",\"preprocessedFeaturesBitExact\":" << (computedFeatures==expectedFeatures?"true":"false") << ",\"replayRepeatable\":true,\"runtimeHeadBitExact\":" << (runtimeHead == captures["head"] ? "true" : "false")
    << ",\"runtimeComposedBitExact\":" << (runtimeOutput == captures["scene-linear-rgba"] ? "true" : "false")
    << ",\"modelManifestSha256\":\"" << modelHash << "\",\"preprocessSpvSha256\":\"" << preHash << "\",\"compositeSpvSha256\":\"" << compositeHash
    << "\",\"shaderSha256\":\"" << kernels.shaderSha256() << "\",\"baselineShaderSha256\":\"" << kernels.baselineShaderSha256() << "\",\"arithmeticPolicy\":\"" << kernels.amdPolicy().arithmeticName()
    << "\",\"publishedHistorySha256\":\"" << digest(captures["published-history"])
    << "\",\"identity\":{\"device_id\":\"" << kernels.deviceId() << "\",\"driver_id\":\"" << kernels.driverId() << "\",\"model_sha256\":\"" << model.manifestSha256() << "\",\"shader_sha256\":\"" << kernels.shaderSha256() << "\",\"baseline_shader_sha256\":\"" << kernels.baselineShaderSha256() << "\"}"
    << ",\"domain\":\"scene-linear RGB in original game exposure domain; no output clamping\",\"cameraResetReproducesFirstOutput\":null,\"cases\":[{\"name\":\"recorded\",\"sourceSha256\":\"" << digest(source)
    << "\",\"historySha256\":\"" << digest(previous) << "\",\"featuresSha256\":\"" << digest(computedFeatures) << "\",\"headSha256\":\"" << digest(captures["head"]) << "\",\"outputSha256\":\"" << digest(captures["scene-linear-rgba"]) << "\"}]}";
  auto text = manifest.str();
  text.pop_back();
  const auto& policy=kernels.amdPolicy();std::ostringstream selected;
  selected << ",\"selected\":{\"kernels\":" << quoteText(kernels.selectedKernelMode())
    << ",\"arithmetic\":" << quoteText(context.isReference()?"reference":policy.arithmeticName()) << ",\"tile_n\":" << policy.tileN << ",\"stage_k\":" << policy.stageK
    << ",\"window_queries\":" << policy.windowQueries << ",\"fusion\":" << (policy.fusion?"true":"false") << ",\"expert_fusion\":" << (policy.expertFusion?"true":"false")
    << ",\"block_fusion\":" << (policy.blockFusion?"true":"false") << ",\"hardware_publication\":" << (policy.hardwarePublication?"true":"false") << "}}";
  text+=selected.str();write(destination / "manifest.json", text.data(), text.size());
  context.destroyPipeline(pre); context.destroyPipeline(composite);
  printf("RECORDED COMPOSITE REPLAY (%s, %s history): %ux%u frame %lld; preprocessing %s captured features; identical replay repeatable; alpha preserved; runtime head %s, runtime composed %s\n",
    vk::backendName(context.backend()),historyMode.c_str(), width, height, (long long)recorded["frame_id"].integer(),computedFeatures==expectedFeatures?"matches":"differs from", runtimeHead == captures["head"] ? "BIT-EXACT" : "DIFFERENT", runtimeOutput == captures["scene-linear-rgba"] ? "BIT-EXACT" : "DIFFERENT");
  return 0;
}
}

int runCompositeValidation(int argc, char** argv) {
  const std::filesystem::path modelPath = argument(argc, argv, "--model"), destination = argument(argc, argv, "--fixture");
  if (modelPath.empty() || destination.empty()) { fprintf(stderr, "usage: dlss5vk compositecheck --backend reference|amd --model <dir> --fixture <dir> [--reference <dir>] [--recorded-frame <capture dir> | --width 320 --height 320]\n"); return 2; }
  const std::filesystem::path referencePath = argument(argc, argv, "--reference");
  check(referencePath.empty() || std::filesystem::absolute(destination).lexically_normal() != std::filesystem::absolute(referencePath).lexically_normal(), "export and reference paths must differ");
  const std::filesystem::path recordedFrame = argument(argc, argv, "--recorded-frame");
  if (!recordedFrame.empty()) return recordedComposite(argc, argv, recordedFrame, modelPath, destination, referencePath);
  const uint32_t width = std::stoul(argument(argc, argv, "--width", "320")), height = std::stoul(argument(argc, argv, "--height", "320"));
  check(width >= 320 && height >= 320 && width <= 3840 && height <= 2160, "invalid scene diagnostic dimensions");
  const auto executable = std::filesystem::path(argv[0]).parent_path();
  const std::string shaderDirectory = argument(argc, argv, "--shaders", (executable / "shaders").string());
  const std::filesystem::path gameShaders = argument(argc, argv, "--game-shaders", (executable / "game/shaders").string());
  std::filesystem::create_directories(destination);
  vk::Context context; check(context.isAmd() || context.isReference(), "scene diagnostic requires AMD/reference backend");
  nr::Model model(context, modelPath.string(), true); nr::Kernels kernels(context, shaderDirectory); kernels.setSiluTable(ref::siluTable());
  auto geometry = nr::Geometry::fromValid(width, height); nr::Graph graph(context, model, kernels, geometry, {.fusedBlocks = false});
  const size_t pixels = size_t(width) * height; Storage storage{context};
  nr::Activation features; features.rows = geometry.fullWidth * geometry.fullHeight; features.allocRows = nr::alignRows(features.rows); features.channels = 16; features.format = nr::Format::F32;
  features.buffer = storage.make(features.validBytes());
  auto input = storage.make((pixels * 2 + 1) * 16), previous = storage.make(pixels * 16), output = storage.make(pixels * 16), nextHistory = storage.make(pixels * 16);
  const auto preSource=shader_identity::read((gameShaders/"game_preprocess.spv").string());
  const auto compositeSource=shader_identity::read((gameShaders/"game_composite.spv").string());
  auto pre=capturedShaderPipeline(context,preSource,"matched game preprocess");
  auto composite=capturedShaderPipeline(context,compositeSource,"matched game composite");
  const auto preHash=captureShaderHash(preSource),compositeHash=captureShaderHash(compositeSource);
  const auto modelHash=captureIdentityHash(model.manifestSha256()); std::ostringstream records; bool first = true;
  for (bool hdr : {false, true}) {
    const auto packed = scene(width, height, hdr); context.upload(input, packed.data(), packed.size() * 16);
    std::map<std::string, Bytes> firstReset;
    for (uint32_t mode = 0; mode < 3; ++mode) {
      const std::string name = std::string(hdr ? "hdr-" : "sdr-") + (mode == 0 ? "reset" : mode == 1 ? "temporal" : "camera-reset");
      const auto oldHistory = history(width, height, mode == 2); context.upload(previous, oldHistory.data(), oldHistory.size() * 16);
      Params params{geometry.fullWidth, geometry.fullHeight, width, height, mode == 1 ? 1u : 0u, mode == 1 ? 1u : 0u, 1, 0, 1, 1, -1, hdr ? 2.0f : 1.0f, 1, num::f16ToF32(nr::auxHalf(model.tensor(70, 0, "blend_scale"), 0, 0)), 1, 1, 1, 4};
      const vk::Buffer* bindings[vk::kGenericBindings]{}; bindings[0] = &input; bindings[1] = &previous; bindings[3] = &output; bindings[4] = &nextHistory; bindings[7] = &features.buffer;
      context.resetDescriptorPool(); dispatch(context, pre, bindings, params, true);
      auto featureBytes = context.download(features.buffer, features.validBytes());
      auto commands = context.beginCommands(); graph.record(commands, features); context.endAndSubmit(commands, true);
      bindings[2] = &graph.head().buffer; dispatch(context, composite, bindings, params, false);
      std::map<std::string, Bytes> captures;
      captures["features"] = std::move(featureBytes); captures["head"] = context.download(graph.head().buffer, graph.head().validBytes());
      captures["scene-linear-rgba"] = context.download(output, pixels * 16); captures["published-history"] = context.download(nextHistory, pixels * 16);
      uint64_t hdrSamples = 0;
      for (size_t pixel = 0; pixel < pixels; ++pixel) {
        Pixel value; memcpy(value.data(), captures["scene-linear-rgba"].data() + pixel * 16, 16);
        for (uint32_t c = 0; c < 4; ++c) check(std::isfinite(value[c]), "nonfinite composite output " + name);
        check(num::f32Bits(value[3]) == num::f32Bits(packed[pixel * 2][3]), "composite changed alpha " + name);
        for (uint32_t c = 0; c < 3; ++c) hdrSamples += value[c] > 1;
      }
      if (hdr) check(hdrSamples != 0, "HDR composite clipped all highlights");
      if (mode == 0) firstReset = captures;
      if (mode == 2) for (const auto& [kind, bytes] : captures) check(bytes == firstReset.at(kind), "camera reset retained prior history in " + kind);
      for (const auto& [kind, bytes] : captures) write(destination / (name + "-" + kind + ".f32"), bytes);
      write(destination / (name + "-source.f32"), packed.data(), packed.size() * 16);
      write(destination / (name + "-previous-history.f32"), oldHistory.data(), oldHistory.size() * 16);
      write(destination / (name + "-controls.bin"), &params, sizeof(params)); pfm(destination / (name + "-scene-linear-rgb.pfm"), captures["scene-linear-rgba"], width, height);
      if (!referencePath.empty()) {
        for (const auto* kind : {"source.f32", "previous-history.f32", "controls.bin", "features.f32"}) {
          const std::string file = name + "-" + kind; check(read(destination / file) == read(referencePath / file), "matched scene/reference input differs: " + file);
        }
      }
      if (!first) records << ','; first = false;
      records << "{\"name\":\"" << name << "\",\"sourceSha256\":\"" << digest(packed) << "\",\"historySha256\":\"" << digest(oldHistory)
              << "\",\"featuresSha256\":\"" << digest(captures["features"]) << "\",\"headSha256\":\"" << digest(captures["head"])
              << "\",\"outputSha256\":\"" << digest(captures["scene-linear-rgba"]) << "\",\"hdrSamplesAboveOne\":" << hdrSamples << '}';
      printf("compositecheck %s: finite scene-linear RGBA, exact alpha, %llu HDR samples >1, shared feature SHA %s\n", name.c_str(), (unsigned long long)hdrSamples, digest(captures["features"]).c_str());
    }
  }
  context.destroyPipeline(pre); context.destroyPipeline(composite);
  std::ostringstream manifest; manifest << "{\"format\":\"OpenNR-local-scene-composite-v1\",\"backend\":\"" << vk::backendName(context.backend())
    << "\",\"width\":" << width << ",\"height\":" << height << ",\"syntheticScene\":true,\"capturedGameFrames\":false,\"nvidiaParityEstablished\":false,\"visualQualityAccepted\":false,"
    << "\"modelManifestSha256\":\"" << modelHash << "\",\"preprocessSpvSha256\":\"" << preHash << "\",\"compositeSpvSha256\":\"" << compositeHash
    << "\",\"domain\":\"scene-linear RGB in original game exposure domain; no output clamping\",\"cameraResetReproducesFirstOutput\":true,\"cases\":[" << records.str() << ']';
  const auto& policy=kernels.amdPolicy();
  manifest << ",\"identity\":{\"device_id\":" << quoteText(kernels.deviceId()) << ",\"driver_id\":" << quoteText(kernels.driverId())
    << ",\"model_sha256\":" << quoteText(model.manifestSha256()) << ",\"shader_sha256\":" << quoteText(kernels.shaderSha256())
    << ",\"baseline_shader_sha256\":" << quoteText(kernels.baselineShaderSha256()) << "},\"selected\":{\"kernels\":" << quoteText(kernels.selectedKernelMode())
    << ",\"arithmetic\":" << quoteText(context.isReference()?"reference":policy.arithmeticName()) << ",\"tile_n\":" << policy.tileN << ",\"stage_k\":" << policy.stageK
    << ",\"window_queries\":" << policy.windowQueries << ",\"fusion\":" << (policy.fusion?"true":"false") << ",\"expert_fusion\":" << (policy.expertFusion?"true":"false")
    << ",\"block_fusion\":" << (policy.blockFusion?"true":"false") << ",\"hardware_publication\":" << (policy.hardwarePublication?"true":"false") << "}}";
  const auto text = manifest.str(); write(destination / "manifest.json", text.data(), text.size());
  printf("COMPOSITE DIAGNOSTIC EXECUTED (%s): shipping game shaders, verified model, six generated scene/history cases; no captured-game or NVIDIA quality acceptance\n", vk::backendName(context.backend())); return 0;
}
