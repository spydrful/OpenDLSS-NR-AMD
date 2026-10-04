// CPU-only route, capability, address/layout and immutable cache regression.
#include "../src/amd_qkv_normalize_validation.h"
#include "../src/amd_selection.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>

namespace {
unsigned checks=0;
void expect(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void rejects(const std::function<void()>& f,const char* message){bool failed=false;try{f();}catch(const std::exception&){failed=true;}expect(failed,message);}
void set(const char* key,const char* value){
#if defined(_WIN32)
  if(_putenv_s(key,value?value:""))throw std::runtime_error("test environment failure");
#else
  if(value?setenv(key,value,1):unsetenv(key))throw std::runtime_error("test environment failure");
#endif
}
constexpr const char* keys[]={"DLSS5VK_AMD_KERNELS","DLSS5VK_AMD_ARITHMETIC","DLSS5VK_AMD_GEMM",
 "DLSS5VK_AMD_TILE_N","DLSS5VK_AMD_STAGE_K","DLSS5VK_AMD_WINDOW_QUERIES","DLSS5VK_AMD_WINDOW_LAYOUT",
 "DLSS5VK_AMD_QKV_NORMALIZE","DLSS5VK_AMD_FUSION","DLSS5VK_AMD_FFN32_FUSION","DLSS5VK_AMD_QKV32_FUSION",
 "DLSS5VK_AMD_EXPERT_FUSION","DLSS5VK_AMD_BLOCK_FUSION","DLSS5VK_AMD_HARDWARE_PUBLICATION","DLSS5VK_AMD_TUNING"};
void clear(){for(const char* k:keys)set(k,nullptr);}
void selectC32(){clear();set("DLSS5VK_AMD_KERNELS","optimized");set("DLSS5VK_AMD_GEMM","direct-rte-pair");
 set("DLSS5VK_AMD_WINDOW_LAYOUT","arena-rte");set("DLSS5VK_AMD_WINDOW_QUERIES","32");set("DLSS5VK_AMD_QKV_NORMALIZE","c32");}
json::Value selected(){return json::parse(R"({"kernels":"optimized","arithmetic":"k16","gemm":"direct-rte-pair",
 "tile_n":16,"stage_k":16,"window_queries":32,"window_layout":"arena-rte","fusion":false,"expert_fusion":false,
 "block_fusion":false,"hardware_publication":false,"ffn32_fusion":false,"qkv32_fusion":false})");}
}
int main(int argc,char** argv){try{
 clear();auto defaults=amd::Options::fromEnvironment();expect(defaults.qkvNormalize==amd::QkvNormalize::Off,"default route changed");
 expect(std::string(defaults.qkvNormalizeName())=="off"&&!defaults.qkvNormalizeC32Enabled(),"default metadata wrong");
 selectC32();auto c32=amd::Options::fromEnvironment();expect(c32.qkvNormalizeC32Enabled()&&std::string(c32.qkvNormalizeName())=="c32","explicit C32 missing");
 expect(!c32.ffn32Enabled()&&!c32.qkv32Enabled()&&!c32.fusion,"new route enabled old shorthand/attention fusion");
 expect(amd::diagnosticSelection(c32),"C32 not diagnostic");amd::Selection lifetime(c32);expect(lifetime.forced(),"C32 not forced");
 set("DLSS5VK_AMD_QKV_NORMALIZE","off");expect(amd::Options::fromEnvironment().qkvNormalize==amd::QkvNormalize::Off&&c32.qkvNormalizeC32Enabled(),"session inherited changed process route");
 lifetime.current().qkvNormalize=amd::QkvNormalize::Off;lifetime.restart();expect(lifetime.current().qkvNormalizeC32Enabled(),"selection restart lost request");
 for(const char* bad:{"1","on","auto","all-c32","ordinary7","C32","c32 "})rejects([&]{amd::Options::parseQkvNormalize(bad);},"bad selector accepted");
 for(auto k:{amd::KernelMode::Auto,amd::KernelMode::Baseline}){auto q=c32;q.kernels=k;rejects([&]{q.validateQkvNormalize();},"non-optimized C32 accepted");}
 for(auto a:{amd::Arithmetic::K32,amd::Arithmetic::Final}){auto q=c32;q.arithmetic=a;rejects([&]{q.validateQkvNormalize();},"experimental C32 accepted");}
 for(auto g:{amd::Gemm::Shared,amd::Gemm::Packed,amd::Gemm::Direct,amd::Gemm::DirectRte,amd::Gemm::DirectRteInit,amd::Gemm::DirectRteEpilogue}){auto q=c32;q.gemm=g;rejects([&]{q.validateQkvNormalize();},"unqualified GEMM C32 accepted");}
 for(auto w:{amd::WindowLayout::Staged,amd::WindowLayout::Register,amd::WindowLayout::RegisterRte}){auto q=c32;q.windowLayout=w;rejects([&]{q.validateQkvNormalize();},"unqualified attention C32 accepted");}
 for(unsigned v:{32u,64u}){auto q=c32;q.tileN=v;rejects([&]{q.validateQkvNormalize();},"tile override accepted");q=c32;q.stageK=v;rejects([&]{q.validateQkvNormalize();},"stage override accepted");}
 for(unsigned v:{16u,64u}){auto q=c32;q.windowQueries=v;rejects([&]{q.validateQkvNormalize();},"query override accepted");}
 for(bool amd::Options::*flag:{&amd::Options::fusion,&amd::Options::ffn32Fusion,&amd::Options::qkv32Fusion,&amd::Options::expertFusion,&amd::Options::blockFusion,&amd::Options::hardwarePublication}){
  auto q=c32;q.*flag=true;rejects([&]{q.validateQkvNormalize();},"fusion/publication override accepted");}
 {auto q=c32;q.tuningPath="qualified.json";rejects([&]{q.validateQkvNormalize();},"unqualified C32 tuning accepted");}
 for(const auto& [key,value]:{std::pair{"DLSS5VK_AMD_KERNELS","auto"},std::pair{"DLSS5VK_AMD_ARITHMETIC","k32"},std::pair{"DLSS5VK_AMD_QKV32_FUSION","1"},std::pair{"DLSS5VK_AMD_TUNING","cache.json"}}){
  selectC32();set(key,value);rejects([]{amd::Options::fromEnvironment();},"invalid environment combination accepted");}
 clear();
 vk::DeviceCapabilities caps;caps.backend=vk::Backend::AmdFast;caps.fp8Matrix16=caps.halfPublicationRte=caps.float32SignedZeroInfNan=true;
 caps.subgroupOperations=VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;caps.properties.limits.maxComputeWorkGroupInvocations=256;
 caps.properties.limits.maxComputeWorkGroupSize[0]=256;caps.properties.limits.maxComputeSharedMemorySize=9216;
 expect(amd::qkvNormalizeSupported(caps),"qualified capability rejected");
 for(auto b:{vk::Backend::Reference,vk::Backend::Nvidia}){auto q=caps;q.backend=b;expect(!amd::qkvNormalizeSupported(q),"wrong backend capable");}
 for(bool vk::DeviceCapabilities::*flag:{&vk::DeviceCapabilities::fp8Matrix16,&vk::DeviceCapabilities::halfPublicationRte,&vk::DeviceCapabilities::float32SignedZeroInfNan}){
  auto q=caps;q.*flag=false;expect(!amd::qkvNormalizeSupported(q),"missing numeric capability accepted");}
 {auto q=caps;q.subgroupOperations=VK_SUBGROUP_FEATURE_SHUFFLE_BIT;expect(!amd::qkvNormalizeSupported(q),"relative shuffle missing");}
 {auto q=caps;q.properties.limits.maxComputeWorkGroupInvocations=255;expect(!amd::qkvNormalizeSupported(q),"invocation limit missing");}
 {auto q=caps;q.properties.limits.maxComputeWorkGroupSize[0]=255;expect(!amd::qkvNormalizeSupported(q),"local X limit missing");}
 {auto q=caps;q.properties.limits.maxComputeSharedMemorySize=9215;expect(!amd::qkvNormalizeSupported(q),"LDS limit missing");}
 for(int b=-1;b<=71;++b)expect(amd::qkvNormalizeBlock(b)==(b==0||b==1||b==2||b==3||b==4||b==66||b==67||b==68||b==69||b==70),"block route range");
 for(int b:{0,1,2,3,4,66,67,68,69,70}){
  unsigned w=b==0?9312:b==66?10400:b==70?8400:8288,s=b==0?20576:b==66?21664:b==70?19664:19552;
  expect(amd::qkvNormalizeLayout(b,w,s),"model layout missing");expect(!amd::qkvNormalizeLayout(b,w+16,s),"bad WQKV layout accepted");expect(!amd::qkvNormalizeLayout(b,w,s+4),"bad scale layout accepted");}
 for(int b:{-1,5,65,71,99})expect(!amd::qkvNormalizeLayout(b,8288,19552),"non-C32 layout accepted");
 for(unsigned r:{1u,17u,73u,25600u,414720u,1658880u,44739199u,44739200u})expect(amd::qkvNormalizeAddressableRows(r),"safe rows rejected");
 for(unsigned r:{0u,44739201u,44739243u,44739244u,UINT32_MAX-63u,UINT32_MAX})expect(!amd::qkvNormalizeAddressableRows(r),"uint addressing wraps");
 auto policy=selected();expect(amd::qkvNormalizePolicy(policy)==amd::QkvNormalize::Off,"legacy missing route not off");
 auto off=c32;off.qkvNormalize=amd::QkvNormalize::Off;amd::requireTuningPolicyMatch(policy,off);++checks;
 rejects([&]{amd::requireTuningPolicyMatch(policy,c32);},"old proof qualifies C32");
 policy.object["qkv_normalize"]=json::parse("\"off\"");amd::requireTuningPolicyMatch(policy,off);++checks;
 policy.object["qkv_normalize"]=json::parse("\"c32\"");amd::requireTuningPolicyMatch(policy,c32);++checks;
 rejects([&]{amd::requireTuningPolicyMatch(policy,off);},"C32 proof qualifies off");
 for(const char* malformed:{"true","1","null","\"on\"","[]","{}"}){auto p=policy;p.object["qkv_normalize"]=json::parse(malformed);rejects([&]{amd::qkvNormalizePolicy(p);},"malformed cache route accepted");}
 expect(argc==2,"CPU suite needs immutable cache path");std::ifstream file(argv[1],std::ios::binary);expect(bool(file),"cache read");
 auto cache=json::parse(std::string((std::istreambuf_iterator<char>(file)),{}));auto old=amd::tuningPolicy(cache,{},1728,960,true);
 expect(old.qkvNormalize==amd::QkvNormalize::Off,"old auto cache enabled normalization");amd::validateTuningRecordPolicies(cache,old);++checks;
 auto poisoned=cache;poisoned.object.at("default_selection").object["qkv_normalize"]=json::parse("\"c32\"");
 rejects([&]{amd::tuningPolicy(poisoned,{},1728,960,true);},"C32 auto cache accepted without proof schema");
 rejects([&]{amd::tuningPolicy(poisoned,c32,1728,960,false);},"forced C32 cache accepted without proof schema");
 poisoned=cache;poisoned.object.at("records").array[0].object.at("evidence").object.at("selected").object["qkv_normalize"]=json::parse("\"c32\"");
 rejects([&]{amd::validateTuningRecordPolicies(poisoned,old);},"C32 operator proof mixed into old cache");
 printf("AMD QKV normalization CPU tests PASS:%u; no Vulkan context or GPU dispatch\n",checks);return 0;
}catch(const std::exception& e){fprintf(stderr,"ERROR:%s\n",e.what());return 1;}}
