#pragma once
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace amd {
enum class KernelMode { Auto, Baseline, Optimized };
enum class Arithmetic { K16, K32, Final };
enum class Gemm { Shared, Packed, Direct, DirectRte, DirectRteInit, DirectRteEpilogue };
enum class WindowLayout { Staged, Register, RegisterRte };
struct Options {
  KernelMode kernels = KernelMode::Auto;
  Arithmetic arithmetic = Arithmetic::K16;
  Gemm gemm = Gemm::Shared;
  WindowLayout windowLayout = WindowLayout::Staged;
  uint32_t tileN = 16, stageK = 16;
  uint32_t windowQueries = 64;
  bool fusion = false, expertFusion = false, blockFusion = false, hardwarePublication = false;
  // fusion is the legacy shorthand/summary for both C32 routes. Per-route
  // environment overrides are resolved once, before the session snapshot.
  bool ffn32Fusion = false, qkv32Fusion = false;
  std::string tuningPath;
  uint32_t publicationInterval() const { return arithmetic == Arithmetic::K16 ? 16u : arithmetic == Arithmetic::K32 ? 32u : 0u; }
  const char* kernelName() const { return kernels == KernelMode::Auto ? "auto" : kernels == KernelMode::Baseline ? "baseline" : "optimized"; }
  const char* arithmeticName() const { return arithmetic == Arithmetic::K16 ? "k16" : arithmetic == Arithmetic::K32 ? "k32" : "final"; }
  static bool scalarRteGemm(Gemm mode) { return mode == Gemm::DirectRte || mode == Gemm::DirectRteInit || mode == Gemm::DirectRteEpilogue; }
  static bool directGemm(Gemm mode) { return mode == Gemm::Direct || scalarRteGemm(mode); }
  bool directOperands() const { return directGemm(gemm); }
  const char* gemmName() const {
    switch(gemm) {
      case Gemm::Shared: return "shared";
      case Gemm::Packed: return "packed";
      case Gemm::Direct: return "direct";
      case Gemm::DirectRte: return "direct-rte";
      case Gemm::DirectRteInit: return "direct-rte-init";
      case Gemm::DirectRteEpilogue: return "direct-rte-epilogue";
    }
    throw std::runtime_error("invalid AMD GEMM selector");
  }
  const char* gemmShaderName() const {
    switch(gemm) {
      case Gemm::Shared: return "amd_gemm_optimized";
      case Gemm::Packed: return "amd_gemm_packed";
      case Gemm::Direct: return "amd_gemm_direct";
      case Gemm::DirectRte: return "amd_gemm_direct_rte";
      case Gemm::DirectRteInit: return "amd_gemm_direct_rte_init";
      case Gemm::DirectRteEpilogue: return "amd_gemm_direct_rte_epilogue";
    }
    throw std::runtime_error("invalid AMD GEMM shader selector");
  }
  bool registerWindowOperands() const { return windowLayout != WindowLayout::Staged; }
  bool requiresRtePublication() const { return scalarRteGemm(gemm) || windowLayout == WindowLayout::RegisterRte; }
  const char* windowLayoutName() const { return windowLayout == WindowLayout::Staged ? "staged" : windowLayout == WindowLayout::Register ? "register" : "register-rte"; }
  const char* windowShaderName() const { return windowLayout == WindowLayout::RegisterRte ? "amd_window_register_rte" : windowLayout == WindowLayout::Register ? "amd_window_register" : windowQueries == 64 ? "amd_window_optimized" : "amd_window_small"; }
  static WindowLayout parseWindowLayout(const std::string& text) {
    if(text=="staged")return WindowLayout::Staged;
    if(text=="register")return WindowLayout::Register;
    if(text=="register-rte")return WindowLayout::RegisterRte;
    throw std::runtime_error("invalid AMD window layout (staged|register|register-rte)");
  }
  static Gemm parseGemm(const std::string& text) {
    if(text=="shared")return Gemm::Shared;
    if(text=="packed")return Gemm::Packed;
    if(text=="direct")return Gemm::Direct;
    if(text=="direct-rte")return Gemm::DirectRte;
    if(text=="direct-rte-init")return Gemm::DirectRteInit;
    if(text=="direct-rte-epilogue")return Gemm::DirectRteEpilogue;
    throw std::runtime_error("invalid AMD GEMM policy (shared|packed|direct|direct-rte|direct-rte-init|direct-rte-epilogue)");
  }
  bool ffn32Enabled() const { return fusion || ffn32Fusion; }
  bool qkv32Enabled() const { return fusion || qkv32Fusion; }
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
    out.gemm = parseGemm(value("DLSS5VK_AMD_GEMM","shared"));
    auto tile = [&](const char* key) {
      const auto text = value(key, "16");
      if (text == "16") return 16u;
      if (text == "32") return 32u;
      if (text == "64") return 64u;
      throw std::runtime_error(std::string("invalid ") + key + " (16|32|64)");
    };
    out.tileN = tile("DLSS5VK_AMD_TILE_N"); out.stageK = tile("DLSS5VK_AMD_STAGE_K");
    if(out.directOperands() && out.stageK!=16)
      throw std::runtime_error("direct AMD GEMM requires DLSS5VK_AMD_STAGE_K=16");
    const auto queries=value("DLSS5VK_AMD_WINDOW_QUERIES","64");
    if(queries=="16")out.windowQueries=16;else if(queries=="32")out.windowQueries=32;else if(queries!="64")throw std::runtime_error("invalid DLSS5VK_AMD_WINDOW_QUERIES (16|32|64)");
    out.windowLayout=parseWindowLayout(value("DLSS5VK_AMD_WINDOW_LAYOUT","staged"));
    if(out.registerWindowOperands() && out.windowQueries==64)
      throw std::runtime_error("register AMD window layout requires DLSS5VK_AMD_WINDOW_QUERIES=16 or 32");
    auto boolean = [&](const char* key) {
      const auto text = value(key, "0");
      if (text != "0" && text != "1") throw std::runtime_error(std::string("invalid ") + key + " (0|1)");
      return text == "1";
    };
    out.fusion = boolean("DLSS5VK_AMD_FUSION");
    auto route = [&](const char* key) {
      const char* text = std::getenv(key);
      return text && *text ? boolean(key) : out.fusion;
    };
    out.ffn32Fusion = route("DLSS5VK_AMD_FFN32_FUSION");
    out.qkv32Fusion = route("DLSS5VK_AMD_QKV32_FUSION");
    out.fusion = out.ffn32Fusion && out.qkv32Fusion;
    out.expertFusion = boolean("DLSS5VK_AMD_EXPERT_FUSION");
    out.blockFusion = boolean("DLSS5VK_AMD_BLOCK_FUSION");
    out.hardwarePublication = boolean("DLSS5VK_AMD_HARDWARE_PUBLICATION");
    if(scalarRteGemm(out.gemm) && out.hardwarePublication)
      throw std::runtime_error("scalar RTE GEMM specifies scalar publication; packed hardware publication must be off");
    out.tuningPath = value("DLSS5VK_AMD_TUNING", "");
    if (out.kernels == KernelMode::Baseline && (out.arithmetic != Arithmetic::K16 || out.gemm != Gemm::Shared || out.windowLayout != WindowLayout::Staged || out.tileN != 16 || out.stageK != 16 || out.windowQueries != 64 || out.ffn32Enabled() || out.qkv32Enabled() || out.expertFusion || out.blockFusion || out.hardwarePublication))
      throw std::runtime_error("baseline AMD kernels require k16, shared GEMM, tile N16/K16 and no fusion/hardware publication override");
    return out;
  }
};
} // namespace amd
