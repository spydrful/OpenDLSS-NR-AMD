// CPU-only tests of process configuration and preserving-baseline boundaries.
// No Vulkan loader, models or GPU work is needed.
#include "../src/amd_config.h"
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {
constexpr const char* names[] = {
  "DLSS5VK_AMD_KERNELS", "DLSS5VK_AMD_ARITHMETIC", "DLSS5VK_AMD_TILE_N", "DLSS5VK_AMD_STAGE_K",
  "DLSS5VK_AMD_WINDOW_QUERIES", "DLSS5VK_AMD_FUSION", "DLSS5VK_AMD_EXPERT_FUSION",
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
    expect(!defaults.fusion && !defaults.expertFusion && !defaults.blockFusion && !defaults.hardwarePublication && defaults.tuningPath.empty(),
           "default configuration enabled unqualified overrides");
    expect(defaults.publicationInterval() == 16, "default publication cadence changed");

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
    set("DLSS5VK_AMD_ARITHMETIC", "final");
    expect(amd::Options::fromEnvironment().publicationInterval() == 0, "final accumulation diagnostic policy was not selected");
    expect(configured.publicationInterval() == 32 && defaults.publicationInterval() == 16,
           "an existing session snapshot changed after process environment edits");

    struct Invalid { const char* name; const char* value; };
    for (const auto& item : std::vector<Invalid>{{"DLSS5VK_AMD_KERNELS", "AUTO"}, {"DLSS5VK_AMD_ARITHMETIC", "fp16"},
        {"DLSS5VK_AMD_TILE_N", "16.0"}, {"DLSS5VK_AMD_TILE_N", "-16"}, {"DLSS5VK_AMD_TILE_N", "128"},
        {"DLSS5VK_AMD_STAGE_K", "32x"}, {"DLSS5VK_AMD_WINDOW_QUERIES", "48"}, {"DLSS5VK_AMD_WINDOW_QUERIES", " 32"},
        {"DLSS5VK_AMD_FUSION", "true"}, {"DLSS5VK_AMD_EXPERT_FUSION", "2"},
        {"DLSS5VK_AMD_BLOCK_FUSION", "-1"}, {"DLSS5VK_AMD_HARDWARE_PUBLICATION", "yes"}}) {
      clear(); set(item.name, item.value);
      rejects([] { (void)amd::Options::fromEnvironment(); }, "invalid process selector was silently accepted");
    }
    for (const auto& item : std::vector<Invalid>{{"DLSS5VK_AMD_ARITHMETIC", "k32"}, {"DLSS5VK_AMD_ARITHMETIC", "final"},
        {"DLSS5VK_AMD_TILE_N", "32"}, {"DLSS5VK_AMD_STAGE_K", "64"}, {"DLSS5VK_AMD_WINDOW_QUERIES", "16"},
        {"DLSS5VK_AMD_WINDOW_QUERIES", "32"}, {"DLSS5VK_AMD_FUSION", "1"}, {"DLSS5VK_AMD_EXPERT_FUSION", "1"},
        {"DLSS5VK_AMD_BLOCK_FUSION", "1"}, {"DLSS5VK_AMD_HARDWARE_PUBLICATION", "1"}}) {
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
