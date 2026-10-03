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
void accepted(bool optimized, uint32_t queries, uint32_t available,
              amd::WindowLayout layout = amd::WindowLayout::Staged) {
  amd::requireWindowLds(optimized, queries, available, "selected AMD attention", layout);
  ++checks;
}
}
int main() {
  try {
    // Independent byte expectations also anchor the exact acceptance boundary.
    struct Variant { bool optimized;uint32_t queries,required;amd::WindowLayout layout=amd::WindowLayout::Staged; };
    for (const auto& variant : {Variant{false,64,34816}, Variant{true,64,22528},
                               Variant{true,32,15360}, Variant{true,16,11776},
                               Variant{true,32,13312,amd::WindowLayout::Register},Variant{true,16,9728,amd::WindowLayout::Register},
                               Variant{true,32,13312,amd::WindowLayout::RegisterRte},Variant{true,16,9728,amd::WindowLayout::RegisterRte}}) {
      expect(amd::windowLdsBytes(variant.optimized,variant.queries,variant.layout)==variant.required,
             "attention resource count changed");
      accepted(variant.optimized,variant.queries,variant.required,variant.layout);
      accepted(variant.optimized,variant.queries,variant.required+1,variant.layout);
      const auto error=rejected([&]{amd::requireWindowLds(variant.optimized,variant.queries,
                                                        variant.required-1,"selected AMD attention",variant.layout);});
      expect(error.find(std::to_string(variant.required))!=std::string::npos,
             "resource rejection omitted required bytes");
      expect(error.find(std::to_string(variant.required-1))!=std::string::npos,
             "resource rejection omitted available bytes");
      accepted(variant.optimized,variant.queries,std::numeric_limits<uint32_t>::max(),variant.layout);
      expect(!rejected([&]{amd::requireWindowLds(variant.optimized,variant.queries,0,
                                                "selected AMD attention",variant.layout);}).empty(),
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
    accepted(true,32,13312,amd::WindowLayout::Register);accepted(true,16,9728,amd::WindowLayout::Register);
    for(const auto layout:{amd::WindowLayout::Register,amd::WindowLayout::RegisterRte}){
      expect(!rejected([&]{amd::requireWindowLds(true,64,UINT32_MAX,"register AMD attention",layout);}).empty(),
             "register Q64 specialization was accepted");
      for(const uint32_t queries:{16u,32u,64u})
        expect(!rejected([&]{amd::requireWindowLds(false,queries,UINT32_MAX,"baseline AMD attention",layout);}).empty(),
               "baseline attention accepted register layout");
    }
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
