// CPU-only tests of process configuration and preserving-baseline boundaries.
// No Vulkan loader, models or GPU work is needed.
#include "../src/amd_config.h"
#include <cstdio>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace {
constexpr const char* names[] = {
  "DLSS5VK_AMD_KERNELS", "DLSS5VK_AMD_ARITHMETIC", "DLSS5VK_AMD_TILE_N", "DLSS5VK_AMD_STAGE_K",
  "DLSS5VK_AMD_WINDOW_QUERIES", "DLSS5VK_AMD_FUSION", "DLSS5VK_AMD_EXPERT_FUSION",
  "DLSS5VK_AMD_FFN32_FUSION", "DLSS5VK_AMD_QKV32_FUSION",
  "DLSS5VK_AMD_GEMM", "DLSS5VK_AMD_WINDOW_LAYOUT",
  "DLSS5VK_AMD_BLOCK_FUSION", "DLSS5VK_AMD_HARDWARE_PUBLICATION", "DLSS5VK_AMD_TUNING"
};
unsigned checks = 0;
void expect(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
void set(const char* name, const char* value) {
#if defined(_WIN32)
  if (_putenv_s(name, value ? value : "")) throw std::runtime_error("cannot set test environment");
#else
  if (value ? setenv(name, value, 1) : unsetenv(name)) throw std::runtime_error("cannot set test environment");
#endif
}
void clear() { for (const char* name : names) set(name, nullptr); }
struct Environment {
  struct Saved { const char* name; bool present; std::string value; };
  std::vector<Saved> saved;
  Environment() {
    for (const char* name : names) {
      const char* value = std::getenv(name);
      saved.push_back({name, value != nullptr, value ? value : ""});
    }
    clear();
  }
  ~Environment() { for (const auto& value : saved) set(value.name, value.present ? value.value.c_str() : nullptr); }
};
void rejects(const std::function<void()>& action, const char* message) {
  bool rejected = false;
  try { action(); } catch (const std::runtime_error&) { rejected = true; }
  expect(rejected, message);
}
}

int main() {
  try {
    Environment environment;
    const auto defaults = amd::Options::fromEnvironment();
    expect(defaults.kernels == amd::KernelMode::Auto && defaults.arithmetic == amd::Arithmetic::K16,
           "default configuration stopped preserving arithmetic");
    expect(defaults.tileN == 16 && defaults.stageK == 16 && defaults.windowQueries == 64,
           "default tile geometry changed");
    expect(!defaults.ffn32Enabled() && !defaults.qkv32Enabled() && !defaults.expertFusion && !defaults.blockFusion && !defaults.hardwarePublication && defaults.tuningPath.empty(),
           "default configuration enabled unqualified overrides");
    expect(defaults.publicationInterval() == 16, "default publication cadence changed");
    expect(defaults.gemm==amd::Gemm::Shared && std::string(defaults.gemmName())=="shared" && std::string(defaults.gemmShaderName())=="amd_gemm_optimized",
           "default GEMM no longer names the qualified shared shader");
    expect(defaults.windowLayout==amd::WindowLayout::Staged && std::string(defaults.windowLayoutName())=="staged" &&
           std::string(defaults.windowShaderName())=="amd_window_optimized","default attention layout/module changed");
    expect(!defaults.registerWindowOperands() && !defaults.requiresRtePublication(),"default policy gained register or RTE requirements");

    set("DLSS5VK_AMD_KERNELS", "optimized");
    set("DLSS5VK_AMD_ARITHMETIC", "k32");
    set("DLSS5VK_AMD_TILE_N", "64"); set("DLSS5VK_AMD_STAGE_K", "32");
    set("DLSS5VK_AMD_WINDOW_QUERIES", "16");
    for (const char* name : {"DLSS5VK_AMD_FUSION", "DLSS5VK_AMD_EXPERT_FUSION", "DLSS5VK_AMD_BLOCK_FUSION", "DLSS5VK_AMD_HARDWARE_PUBLICATION"}) set(name, "1");
    set("DLSS5VK_AMD_TUNING", "diagnostic tuning file.json");
    const auto configured = amd::Options::fromEnvironment();
    expect(configured.kernels == amd::KernelMode::Optimized && configured.arithmetic == amd::Arithmetic::K32 && configured.publicationInterval() == 32,
           "explicit K32 diagnostic policy was not retained");
    expect(configured.tileN == 64 && configured.stageK == 32 && configured.windowQueries == 16,
           "explicit session geometry was not retained");
    expect(configured.fusion && configured.expertFusion && configured.blockFusion && configured.hardwarePublication &&
           configured.tuningPath == "diagnostic tuning file.json", "explicit capabilities/path were not retained");
    expect(configured.ffn32Enabled() && configured.qkv32Enabled(),"legacy fusion did not enable both routes");
    set("DLSS5VK_AMD_ARITHMETIC", "final");
    expect(amd::Options::fromEnvironment().publicationInterval() == 0, "final accumulation diagnostic policy was not selected");
    expect(configured.publicationInterval() == 32 && defaults.publicationInterval() == 16,
           "an existing session snapshot changed after process environment edits");

    // Independent 0|1 values override the legacy shorthand in every
    // combination. Empty/unset values inherit it, as other selectors do.
    for(const char* legacy:{"0","1"})for(const char* ffn:{"","0","1"})for(const char* qkv:{"","0","1"}){
      clear();set("DLSS5VK_AMD_FUSION",legacy);set("DLSS5VK_AMD_FFN32_FUSION",ffn);set("DLSS5VK_AMD_QKV32_FUSION",qkv);
      const auto routes=amd::Options::fromEnvironment();
      const bool expectedFfn=(*ffn ? *ffn : *legacy)=='1',expectedQkv=(*qkv ? *qkv : *legacy)=='1';
      expect(routes.ffn32Enabled()==expectedFfn && routes.qkv32Enabled()==expectedQkv && routes.fusion==(expectedFfn&&expectedQkv),
             "independent route did not override shorthand or aggregate summary");
    }
    clear();set("DLSS5VK_AMD_FFN32_FUSION","1");const auto independent=amd::Options::fromEnvironment();
    set("DLSS5VK_AMD_FFN32_FUSION","0");set("DLSS5VK_AMD_QKV32_FUSION","1");
    expect(independent.ffn32Enabled() && !independent.qkv32Enabled(),"environment changes mutated an independent session snapshot");
    for(const auto& [name,mode,shader]:std::vector<std::tuple<const char*,amd::Gemm,const char*>>{{"shared",amd::Gemm::Shared,"amd_gemm_optimized"},{"packed",amd::Gemm::Packed,"amd_gemm_packed"},{"direct",amd::Gemm::Direct,"amd_gemm_direct"},{"direct-rte",amd::Gemm::DirectRte,"amd_gemm_direct_rte"},{"direct-rte-init",amd::Gemm::DirectRteInit,"amd_gemm_direct_rte_init"},{"direct-rte-epilogue",amd::Gemm::DirectRteEpilogue,"amd_gemm_direct_rte_epilogue"}}){
      clear();set("DLSS5VK_AMD_GEMM",name);const auto selected=amd::Options::fromEnvironment();
      expect(selected.gemm==mode && std::string(selected.gemmName())==name && std::string(selected.gemmShaderName())==shader,
             "GEMM selector/name/shader identity differed");
      const bool rte=mode==amd::Gemm::DirectRte || mode==amd::Gemm::DirectRteInit || mode==amd::Gemm::DirectRteEpilogue;
      expect(selected.requiresRtePublication()==rte,"GEMM policy lost its independent RTE capability requirement");
      expect(selected.directOperands()==(mode==amd::Gemm::Direct || rte),"direct GEMM policy gained operand staging");
      set("DLSS5VK_AMD_GEMM","shared");expect(selected.gemm==mode,"an existing GEMM selection changed with the environment");
    }
    for(const char* stage:{"32","64"}){
      for(const char* name:{"direct","direct-rte","direct-rte-init","direct-rte-epilogue"}){
        clear();set("DLSS5VK_AMD_GEMM",name);set("DLSS5VK_AMD_STAGE_K",stage);
        rejects([]{(void)amd::Options::fromEnvironment();},"direct GEMM falsely accepted nonexistent staging");
      }
      set("DLSS5VK_AMD_GEMM","packed");expect(amd::Options::fromEnvironment().stageK==uint32_t(std::stoi(stage)),"packed GEMM staging was restricted");
    }
    for(const char* name:{"direct-rte","direct-rte-init","direct-rte-epilogue"}){
      clear();set("DLSS5VK_AMD_GEMM",name);set("DLSS5VK_AMD_HARDWARE_PUBLICATION","1");
      rejects([]{(void)amd::Options::fromEnvironment();},"scalar RTE was mislabeled as packed hardware publication");
      for(const char* tile:{"16","32","64"}){
        clear();set("DLSS5VK_AMD_GEMM",name);set("DLSS5VK_AMD_TILE_N",tile);
        const auto candidate=amd::Options::fromEnvironment();
        expect(candidate.tileN==uint32_t(std::stoi(tile)) && candidate.stageK==16 && !candidate.hardwarePublication,
               "scalar RTE tile experiment changed staging or packed publication");
      }
    }

    for(const char* layout:{"register","register-rte"})for(const char* queries:{"16","32"})for(const char* arithmetic:{"k16","k32","final"}){
      clear();set("DLSS5VK_AMD_WINDOW_QUERIES",queries);set("DLSS5VK_AMD_ARITHMETIC",arithmetic);
      const auto staged=amd::Options::fromEnvironment();
      expect(staged.windowLayout==amd::WindowLayout::Staged && std::string(staged.windowShaderName())=="amd_window_small",
             "legacy small-query attention no longer selects its original module");
      set("DLSS5VK_AMD_WINDOW_LAYOUT",layout);const auto reg=amd::Options::fromEnvironment();
      const auto expected=amd::Options::parseWindowLayout(layout);
      const char* shader=expected==amd::WindowLayout::Register ? "amd_window_register" : "amd_window_register_rte";
      expect(reg.windowLayout==expected && std::string(reg.windowLayoutName())==layout &&
             std::string(reg.windowShaderName())==shader && reg.windowQueries==uint32_t(std::stoi(queries)),
             "register attention selector lost its layout/module/query geometry");
      expect(reg.registerWindowOperands() && reg.requiresRtePublication()==(expected==amd::WindowLayout::RegisterRte),
             "attention policy lost its independent storage or RTE capability requirement");
      expect(reg.arithmetic==staged.arithmetic,"window layout changed the selected arithmetic policy");
      set("DLSS5VK_AMD_WINDOW_LAYOUT","staged");set("DLSS5VK_AMD_WINDOW_QUERIES","64");
      expect(reg.windowLayout==expected && std::string(reg.windowShaderName())==shader,
             "process edits mutated an existing window-layout session snapshot");
    }
    for(const char* layout:{"register","register-rte"}){
      clear();set("DLSS5VK_AMD_WINDOW_LAYOUT",layout);
      rejects([]{(void)amd::Options::fromEnvironment();},"register attention accepted an unsupported Q64 specialization");
    }
    clear();set("DLSS5VK_AMD_WINDOW_QUERIES","32");set("DLSS5VK_AMD_WINDOW_LAYOUT","register");set("DLSS5VK_AMD_GEMM","direct-rte");
    const auto combined=amd::Options::fromEnvironment();
    expect(combined.gemm==amd::Gemm::DirectRte && combined.windowLayout==amd::WindowLayout::Register && combined.stageK==16 &&
           !combined.hardwarePublication,"register attention changed scalar RTE constraints");
    clear();set("DLSS5VK_AMD_WINDOW_QUERIES","32");set("DLSS5VK_AMD_WINDOW_LAYOUT","register-rte");set("DLSS5VK_AMD_GEMM","direct");
    const auto independentRte=amd::Options::fromEnvironment();
    expect(independentRte.gemm==amd::Gemm::Direct && independentRte.windowLayout==amd::WindowLayout::RegisterRte && independentRte.requiresRtePublication(),
           "RTE attention depended on a GEMM RTE override for its capability gate");

    struct Invalid { const char* name; const char* value; };
    for (const auto& item : std::vector<Invalid>{{"DLSS5VK_AMD_KERNELS", "AUTO"}, {"DLSS5VK_AMD_ARITHMETIC", "fp16"},
        {"DLSS5VK_AMD_TILE_N", "16.0"}, {"DLSS5VK_AMD_TILE_N", "-16"}, {"DLSS5VK_AMD_TILE_N", "128"},
        {"DLSS5VK_AMD_STAGE_K", "32x"}, {"DLSS5VK_AMD_WINDOW_QUERIES", "48"}, {"DLSS5VK_AMD_WINDOW_QUERIES", " 32"},
        {"DLSS5VK_AMD_FUSION", "true"}, {"DLSS5VK_AMD_FFN32_FUSION", "true"}, {"DLSS5VK_AMD_QKV32_FUSION", "2"}, {"DLSS5VK_AMD_EXPERT_FUSION", "2"},
        {"DLSS5VK_AMD_BLOCK_FUSION", "-1"}, {"DLSS5VK_AMD_HARDWARE_PUBLICATION", "yes"},
        {"DLSS5VK_AMD_GEMM","DIRECT"},{"DLSS5VK_AMD_GEMM","direct-global"},{"DLSS5VK_AMD_GEMM"," packed"},
        {"DLSS5VK_AMD_WINDOW_LAYOUT","REGISTER"},{"DLSS5VK_AMD_WINDOW_LAYOUT"," register"},{"DLSS5VK_AMD_WINDOW_LAYOUT","packed"},
        {"DLSS5VK_AMD_WINDOW_LAYOUT","REGISTER-RTE"},{"DLSS5VK_AMD_WINDOW_LAYOUT","register_rte"}}) {
      clear(); set(item.name, item.value);
      rejects([] { (void)amd::Options::fromEnvironment(); }, "invalid process selector was silently accepted");
    }
    for (const auto& item : std::vector<Invalid>{{"DLSS5VK_AMD_ARITHMETIC", "k32"}, {"DLSS5VK_AMD_ARITHMETIC", "final"},
        {"DLSS5VK_AMD_TILE_N", "32"}, {"DLSS5VK_AMD_STAGE_K", "64"}, {"DLSS5VK_AMD_WINDOW_QUERIES", "16"},
        {"DLSS5VK_AMD_WINDOW_QUERIES", "32"}, {"DLSS5VK_AMD_FUSION", "1"}, {"DLSS5VK_AMD_FFN32_FUSION", "1"}, {"DLSS5VK_AMD_QKV32_FUSION", "1"}, {"DLSS5VK_AMD_EXPERT_FUSION", "1"},
        {"DLSS5VK_AMD_BLOCK_FUSION", "1"}, {"DLSS5VK_AMD_HARDWARE_PUBLICATION", "1"},
        {"DLSS5VK_AMD_GEMM","packed"},{"DLSS5VK_AMD_GEMM","direct"},{"DLSS5VK_AMD_GEMM","direct-rte"},
        {"DLSS5VK_AMD_GEMM","direct-rte-init"},{"DLSS5VK_AMD_GEMM","direct-rte-epilogue"},
        {"DLSS5VK_AMD_WINDOW_LAYOUT","register"},{"DLSS5VK_AMD_WINDOW_LAYOUT","register-rte"}}) {
      clear(); set("DLSS5VK_AMD_KERNELS", "baseline"); set(item.name, item.value);
      rejects([] { (void)amd::Options::fromEnvironment(); }, "frozen baseline accepted an arithmetic/kernel override");
    }
    clear(); set("DLSS5VK_AMD_KERNELS", "baseline"); set("DLSS5VK_AMD_TUNING", "unread tuning path");
    expect(amd::Options::fromEnvironment().kernels == amd::KernelMode::Baseline,
           "a tuning filename prevented explicit baseline selection");
    printf("AMD process configuration: %u CPU checks PASS\n", checks);
    return 0;
  } catch (const std::exception& error) {
    fprintf(stderr, "AMD process configuration FAIL: %s\n", error.what()); return 1;
  }
}
