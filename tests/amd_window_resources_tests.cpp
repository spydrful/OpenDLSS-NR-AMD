// CPU-only resource-limit regression used by the live AMD dispatch guard.
#include "../src/amd_window_resources.h"
#include <cstdio>
#include <functional>
#include <limits>

namespace {
unsigned checks = 0;
void expect(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
std::string rejected(const std::function<void()>& action) {
  try { action(); } catch (const std::runtime_error& error) { return error.what(); }
  throw std::runtime_error("over-limit or invalid attention selection was accepted");
}
void accepted(bool optimized, uint32_t queries, uint32_t available) {
  amd::requireWindowLds(optimized, queries, available, "selected AMD attention");
  ++checks;
}
}
int main() {
  try {
    // Independent byte expectations also anchor the exact acceptance boundary.
    struct Variant { bool optimized;uint32_t queries,required; };
    for (const auto& variant : {Variant{false,64,34816}, Variant{true,64,22528},
                               Variant{true,32,15360}, Variant{true,16,11776}}) {
      expect(amd::windowLdsBytes(variant.optimized,variant.queries)==variant.required,
             "attention resource count changed");
      accepted(variant.optimized,variant.queries,variant.required);
      accepted(variant.optimized,variant.queries,variant.required+1);
      const auto error=rejected([&]{amd::requireWindowLds(variant.optimized,variant.queries,
                                                        variant.required-1,"selected AMD attention");});
      expect(error.find(std::to_string(variant.required))!=std::string::npos,
             "resource rejection omitted required bytes");
      expect(error.find(std::to_string(variant.required-1))!=std::string::npos,
             "resource rejection omitted available bytes");
      accepted(variant.optimized,variant.queries,std::numeric_limits<uint32_t>::max());
      expect(!rejected([&]{amd::requireWindowLds(variant.optimized,variant.queries,0,
                                                "selected AMD attention");}).empty(),
             "zero-capacity device was accepted");
    }
    const auto legacy=rejected([]{amd::requireWindowLds(false,64,32768,
                                                       "forced legacy AMD baseline attention");});
    expect(legacy=="forced legacy AMD baseline attention requires 34816 bytes shared memory; device provides 32768 bytes",
           "forced baseline failure does not identify the actual limit");
    for (const uint32_t queries : {16u,32u,64u}) accepted(true,queries,32768);
    expect(!rejected([]{amd::requireWindowLds(true,64,18432,"compact AMD attention");}).empty(),
           "Q64 was accepted on a smaller device");
    accepted(true,32,18432);accepted(true,16,18432);
    for (const uint32_t invalid : {0u,1u,15u,17u,31u,33u,63u,65u,UINT32_MAX})
      expect(!rejected([&]{amd::requireWindowLds(true,invalid,UINT32_MAX,
                                                "compact AMD attention");}).empty(),
             "invalid compact query specialization was accepted");
    printf("AMD window resource limits: %u CPU checks PASS\n",checks);
    return 0;
  } catch (const std::exception& error) {
    fprintf(stderr,"AMD window resource limits FAIL: %s\n",error.what());return 1;
  }
}
