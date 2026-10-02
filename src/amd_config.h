#pragma once
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace amd {
enum class KernelMode { Auto, Baseline, Optimized };
enum class Arithmetic { K16, K32, Final };
struct Options {
  KernelMode kernels = KernelMode::Auto;
  Arithmetic arithmetic = Arithmetic::K16;
  uint32_t tileN = 16, stageK = 16;
  uint32_t windowQueries = 64;
  bool fusion = false, expertFusion = false, blockFusion = false, hardwarePublication = false;
  std::string tuningPath;
  uint32_t publicationInterval() const { return arithmetic == Arithmetic::K16 ? 16u : arithmetic == Arithmetic::K32 ? 32u : 0u; }
  const char* kernelName() const { return kernels == KernelMode::Auto ? "auto" : kernels == KernelMode::Baseline ? "baseline" : "optimized"; }
  const char* arithmeticName() const { return arithmetic == Arithmetic::K16 ? "k16" : arithmetic == Arithmetic::K32 ? "k32" : "final"; }
  static Options fromEnvironment() {
    Options out;
    auto value = [](const char* key, const char* fallback) { const char* v = std::getenv(key); return std::string(v && *v ? v : fallback); };
    const auto mode = value("DLSS5VK_AMD_KERNELS", "auto");
    if (mode == "baseline") out.kernels = KernelMode::Baseline;
    else if (mode == "optimized") out.kernels = KernelMode::Optimized;
    else if (mode != "auto") throw std::runtime_error("invalid DLSS5VK_AMD_KERNELS (auto|baseline|optimized)");
    const auto arithmetic = value("DLSS5VK_AMD_ARITHMETIC", "k16");
    if (arithmetic == "k32") out.arithmetic = Arithmetic::K32;
    else if (arithmetic == "final") out.arithmetic = Arithmetic::Final;
    else if (arithmetic != "k16") throw std::runtime_error("invalid DLSS5VK_AMD_ARITHMETIC (k16|k32|final)");
    auto tile = [&](const char* key) {
      const auto text = value(key, "16");
      if (text == "16") return 16u;
      if (text == "32") return 32u;
      if (text == "64") return 64u;
      throw std::runtime_error(std::string("invalid ") + key + " (16|32|64)");
    };
    out.tileN = tile("DLSS5VK_AMD_TILE_N"); out.stageK = tile("DLSS5VK_AMD_STAGE_K");
    const auto queries=value("DLSS5VK_AMD_WINDOW_QUERIES","64");
    if(queries=="16")out.windowQueries=16;else if(queries=="32")out.windowQueries=32;else if(queries!="64")throw std::runtime_error("invalid DLSS5VK_AMD_WINDOW_QUERIES (16|32|64)");
    auto boolean = [&](const char* key) {
      const auto text = value(key, "0");
      if (text != "0" && text != "1") throw std::runtime_error(std::string("invalid ") + key + " (0|1)");
      return text == "1";
    };
    out.fusion = boolean("DLSS5VK_AMD_FUSION");
    out.expertFusion = boolean("DLSS5VK_AMD_EXPERT_FUSION");
    out.blockFusion = boolean("DLSS5VK_AMD_BLOCK_FUSION");
    out.hardwarePublication = boolean("DLSS5VK_AMD_HARDWARE_PUBLICATION");
    out.tuningPath = value("DLSS5VK_AMD_TUNING", "");
    if (out.kernels == KernelMode::Baseline && (out.arithmetic != Arithmetic::K16 || out.tileN != 16 || out.stageK != 16 || out.windowQueries != 64 || out.fusion || out.expertFusion || out.blockFusion || out.hardwarePublication))
      throw std::runtime_error("baseline AMD kernels require k16, tile N16/K16 and no fusion/hardware publication override");
    return out;
  }
};
} // namespace amd
