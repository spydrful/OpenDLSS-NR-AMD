// Standalone C32 QKV -> normalization diagnostic. MIT; no shipping routes.
// Baseline core/model implementation is MIT; proprietary assets remain private.
#include "vk_context.h"
#include "nr_model.h"
#include "numeric.h"
#include "sha256.h"
#include "json.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
namespace fs=std::filesystem;
constexpr const char* kPair="74ebd099535caf03e2875ee8d8345d0c1bc311e6133fcde15190b79301e86c1e";
constexpr const char* kNormalize="c5c8161b72282e283b5ab108f732a328731da39fb5d02c2500a9230b754fe008";
constexpr const char* kProduction="9b1c585054bbbe3c9c2021d393699eae16e9ffcf889db6311d070d0049be5cc5";
constexpr const char* kCapture="313536a959921d0c444ae412783189509e4ba293cd961d244ee9b6cc3c8b33df";
constexpr const char* kKnownModelHash="163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e";
constexpr const char* kKnownDriver="AMD proprietary driver|26.9.1 (LLPC)|8389003";
constexpr const char* kKnownDevice="AMD Radeon RX 9070 XT";
constexpr const char* kAnchorAggregate="917ff3fd9e14f041c4172d047026fa35319eb3e87f3b9deaff3579af637f3436";
constexpr const char* kSelected=R"({"kernels":"optimized","arithmetic":"k16","gemm":"direct-rte-pair","tile_n":16,"stage_k":16,"window_queries":32,"window_layout":"arena-rte","qkv_normalize":"c32","fusion":false,"ffn32_fusion":false,"qkv32_fusion":false,"expert_fusion":false,"block_fusion":false,"hardware_publication":false})";
unsigned strictProofCpuChecks();
// Explicit offsets from immutable core FusedLayout/pre/upsample/post layouts.
std::pair<uint32_t,uint32_t> blockOffsets(int block) {
 if(block==0)return {9312,20576};
 if(block==66)return {10400,21664};
 if(block==70)return {8400,19664};
 return {8288,19552};
}
const std::array<int,10> kBlocks={0,1,2,3,4,66,67,68,69,70};
void require(bool ok,const std::string& what){if(!ok)throw std::runtime_error(what);}
uint32_t align64(uint32_t x){return (x+63u)&~63u;}
std::string hash(const std::vector<uint8_t>& v){auto s=sha256Hex(v.data(),v.size());for(auto& c:s)if(c>='A'&&c<='F')c+=32;return s;}
std::vector<uint8_t> read(const fs::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);require(bool(f),"cannot read "+p.string());auto n=f.tellg();require(n>=0,"invalid size");std::vector<uint8_t> v(size_t(n),0);f.seekg(0);if(!v.empty())f.read(reinterpret_cast<char*>(v.data()),v.size());require(bool(f),"short read");return v;}
void write(const fs::path& p,const std::vector<uint8_t>& v){std::ofstream f(p,std::ios::binary);require(bool(f),"cannot write "+p.string());if(!v.empty())f.write(reinterpret_cast<const char*>(v.data()),v.size());require(bool(f),"short write");}
std::string quote(const std::string& s){std::ostringstream o;o<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')o<<'\\'<<char(c);else if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<char(c);}return o.str()+"\"";}
template<class T>std::vector<uint8_t> bytes(const std::vector<T>& v){const auto* p=reinterpret_cast<const uint8_t*>(v.data());return {p,p+v.size()*sizeof(T)};}
uint32_t integer(const json::Value& v,uint32_t minimum,uint32_t maximum){require(v.kind==json::Value::Number&&v.number>=minimum&&v.number<=maximum&&std::floor(v.number)==v.number,"invalid integer");return uint32_t(v.number);}
bool validBlock(int block){return std::find(kBlocks.begin(),kBlocks.end(),block)!=kBlocks.end();}
struct Fixture{
 std::string name;uint32_t rows=0,stride=32,inputBase=0,weightBase=0,scale=0;
 bool overdispatch=false,actualWeights=false,actualInput=false;int block=-1;
 std::vector<uint8_t> input,weights;
 std::string inputSourceSha256,inputBoundary,sourceManifestSha256;
 std::string referenceQkvSha256,referenceNormalizedSha256;
 void validate()const{
  require(rows&&rows<=1658880,"rows outside bounded target");
  require(stride>=32&&stride<=256&&stride%4==0&&inputBase%4==0&&inputBase+32<=stride,"invalid input stride/base");
  require(weightBase<=32&&weightBase%16==0,"invalid weight base");
  require(input.size()==size_t(align64(rows))*stride,"input allocation mismatch");
  require(weights.size()==size_t(weightBase+96)*32,"weight allocation mismatch");
  if(actualWeights)require(validBlock(block)&&weightBase==0,"actual tensor scope");
  if(actualInput)require(actualWeights&&inputBoundary=="ffn_quantized"&&!inputSourceSha256.empty()&&!sourceManifestSha256.empty(),"actual input boundary missing");
 }
 std::string identity()const{auto v=input;v.insert(v.end(),weights.begin(),weights.end());for(uint32_t x:{rows,stride,inputBase,weightBase,scale,uint32_t(overdispatch),uint32_t(actualWeights),uint32_t(actualInput),uint32_t(block)})for(unsigned b=0;b<4;++b)v.push_back(uint8_t(x>>(8*b)));return hash(v);}
};
Fixture fixture(std::string name,uint32_t rows,uint32_t stride=32,uint32_t inputBase=0,uint32_t weightBase=0){
 Fixture f;f.name=std::move(name);f.rows=rows;f.stride=stride;f.inputBase=inputBase;f.weightBase=weightBase;f.scale=num::f32Bits(.8125f);
 f.input.resize(size_t(align64(rows))*stride);f.weights.resize(size_t(weightBase+96)*32);
 uint32_t seed=0x9070a195u+rows+stride+inputBase+weightBase;
 for(auto& x:f.input){seed=seed*1664525u+1013904223u;x=num::e4m3FromF32(float(int(seed%65)-32)/64.f);}
 for(auto& x:f.weights){seed=seed*1664525u+1013904223u;x=num::e4m3FromF32(float(int(seed%65)-32)/64.f);}
 f.validate();return f;
}
void diagonal(Fixture& f,float q=1.f,float k=1.f,float v=1.f){
 std::fill(f.weights.begin(),f.weights.end(),uint8_t(0));
 for(uint32_t n=0;n<96;++n)f.weights[(f.weightBase+n)*32+n%32]=num::e4m3FromF32(n<32?q:n<64?k:v);
}
void fillInput(Fixture& f,uint8_t code){for(uint32_t r=0;r<align64(f.rows);++r)for(uint32_t k=0;k<32;++k)f.input[r*f.stride+f.inputBase+k]=code;}
std::vector<Fixture> synthetic(bool target){
 std::vector<Fixture> out;
 for(uint32_t stride:{32u,48u,64u,96u})for(uint32_t rows:{1u,2u,15u,16u,17u,31u,32u,33u,63u,64u,65u,73u,127u,128u,129u,257u})out.push_back(fixture("finite-R"+std::to_string(rows)+"-S"+std::to_string(stride),rows,stride));
 for(uint32_t ib:{4u,8u,16u})for(uint32_t wb:{16u,32u})out.push_back(fixture("offset-I"+std::to_string(ib)+"-W"+std::to_string(wb),73,64,ib,wb));
 for(uint32_t rows:{1u,17u,73u}){auto f=fixture("overdispatch-R"+std::to_string(rows),rows,48);f.overdispatch=true;out.push_back(std::move(f));}
 auto grid=fixture("all-e4-input-identity",8);diagonal(grid);for(uint32_t r=0;r<8;++r)for(uint32_t k=0;k<32;++k)grid.input[r*32+k]=uint8_t(r*32+k);out.push_back(grid);
 auto weights=fixture("all-e4-weight-grid",73);fillInput(weights,0x38);for(size_t i=0;i<weights.weights.size();++i)weights.weights[i]=uint8_t(i);out.push_back(weights);
 for(uint8_t code:{uint8_t(0),uint8_t(0x80),uint8_t(1),uint8_t(0x81),uint8_t(0x7e),uint8_t(0xfe),uint8_t(0x7f),uint8_t(0xff)}){
  auto f=fixture("identity-E4-"+std::to_string(code),73);diagonal(f);fillInput(f,code);out.push_back(std::move(f));
 }
 auto mixed=fixture("mixed-Q-small-K-final-overflow",73);diagonal(mixed,.140625f,-1.5f,.015625f);fillInput(mixed,num::e4m3FromF32(32.f));out.push_back(mixed);
 auto dense=fixture("dense-matrix-half-overflow",73);fillInput(dense,0x7e);std::fill(dense.weights.begin(),dense.weights.end(),uint8_t(0x7e));out.push_back(dense);
 auto oppos=fixture("dense-alternating-cancellation",73);fillInput(oppos,0x7e);for(size_t i=0;i<oppos.weights.size();++i)oppos.weights[i]=i&1?0xfe:0x7e;out.push_back(oppos);
 constexpr uint16_t halfCodes[]={0,0x8000,1,0x8001,0x03ff,0x83ff,0x0400,0x8400,0x1400,0x9400,0x1800,0x9800,0x3bff,0x3c00,0x3c01,0x5400,0x5bff,0x5c00,0x7bff,0xfbff,0x7c00,0xfc00,0x7e00,0xfe00,0x7d55,0xfd55};
 for(uint16_t code:halfCodes){auto f=mixed;f.name="mixed-overflow-scale-half-"+std::to_string(code);f.scale=num::f32Bits(num::f16ToF32(code));out.push_back(std::move(f));}
 for(unsigned seed=0;seed<8;++seed){auto f=fixture("mixed-E4-grid-"+std::to_string(seed),73);for(uint32_t r=0;r<align64(f.rows);++r)for(uint32_t c=0;c<32;++c)f.input[r*32+c]=uint8_t((r*32+c)^((seed*37)&255));for(size_t i=0;i<f.weights.size();++i)f.weights[i]=uint8_t((i*13+seed*41)&255);out.push_back(std::move(f));}
 if(target)for(uint32_t r:{414720u,1658880u})out.push_back(fixture("target-R"+std::to_string(r),r));
 for(const auto& f:out)f.validate();return out;
}
void loadWeights(Fixture& f,nr::Model& model,int block){
 require(validBlock(block),"unsupported ordinary block");const auto& tensor=model.tensor(block);
 require(tensor.byteLength>=blockOffsets(block).second+4,"ordinary QKV tensor too short");
 f.weights=model.fp8MatrixBytes(tensor,blockOffsets(block).first,32,96,true,0,true);
 f.scale=num::f32Bits(nr::auxF32(tensor,blockOffsets(block).second));f.actualWeights=true;f.block=block;f.validate();
}
std::vector<Fixture> actualWeights(nr::Model& model,bool target){
 std::vector<Fixture> out;for(int block:kBlocks)for(uint32_t rows:{1u,73u}){auto f=fixture("actual-weights-block"+std::to_string(block)+"-R"+std::to_string(rows),rows,48);loadWeights(f,model,block);out.push_back(std::move(f));}
 if(target){auto f=fixture("actual-weights-block1-target-R414720",414720);loadWeights(f,model,1);out.push_back(std::move(f));}return out;
}
struct CaptureEntry{int block;uint32_t rows,stride,inputBase;fs::path file;std::string sha256;};
CaptureEntry captureEntry(const json::Value& entry){
 require(entry["input_boundary"].str()=="ffn_quantized","must capture actual QKV E4 input, not block input");
 require(entry["layout"].str()=="row-major-e4","unsupported activation layout");
 CaptureEntry c;c.block=int(integer(entry["block"],0,74));require(validBlock(c.block),"actual capture block outside bounded C32 ordinary/full/transition scope");
 c.rows=integer(entry["rows"],1,1658880);c.stride=integer(entry["input_stride"],32,256);c.inputBase=integer(entry["input_column_base"],0,224);
 require(c.stride%4==0&&c.inputBase%4==0&&c.inputBase+32<=c.stride,"capture input stride/base invalid");
 require(integer(entry["padded_rows"],1,1658880)==align64(c.rows),"capture row padding mismatch");
 require(integer(entry["byte_length"],1,425000000)==uint64_t(align64(c.rows))*c.stride,"input capture length mismatch");
 require(entry["file"].kind==json::Value::String&&!entry["file"].str().empty(),"capture filename required");c.file=entry["file"].str();
 require(entry["sha256"].kind==json::Value::String,"capture SHA string required");c.sha256=entry["sha256"].str();
 require(c.sha256.size()==64&&std::all_of(c.sha256.begin(),c.sha256.end(),[](char x){return(x>='0'&&x<='9')||(x>='a'&&x<='f');}),"capture SHA must be lower-case hex");
 return c;
}
bool shaString(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
json::Value readJson(const fs::path& path){auto data=read(path);return json::parse(std::string(data.begin(),data.end()));}
void trueField(const json::Value& v,const char* key){require(v[key].kind==json::Value::Bool&&v[key].boolean,std::string("missing true proof field: ")+key);}
void localFilename(const fs::path& path){
 const auto name=path.string();
 require(!name.empty()&&!path.is_absolute()&&!path.has_root_path()&&path.parent_path().empty()&&
  name!="."&&name!=".."&&name.find(':')==std::string::npos&&name.find('\\')==std::string::npos&&name.find('/')==std::string::npos,
  "catalog filenames must be simple local filenames");
}
fs::path localFile(const fs::path& catalog,const fs::path& relative){
 localFilename(relative);auto base=fs::canonical(catalog.parent_path()),file=fs::canonical(base/relative);
 require(fs::is_regular_file(file)&&file.parent_path()==base,"catalog file escapes its directory or is not regular");return file;
}
uint32_t expectedRows(int block){require(validBlock(block),"unsupported C32 block");return block==0||block==70?1658880u:414720u;}
void catalogMetadata(const json::Value& record){
 require(record["format"].str()=="OpenNR-c32-qkv-ffn-inputs-v1","unsupported actual input format");
 require(record["model_manifest_sha256"].str()==kKnownModelHash,"actual input model mismatch");
 require(record["baseline_policy"].str()=="Pair-Arena-Q32-K16-off"&&record["source_shader_sha256"].str()==kAnchorAggregate,"capture anchor policy/module mismatch");
 require(integer(record["valid_width"],1,32768)==1707&&integer(record["valid_height"],1,32768)==960&&integer(record["padded_width"],1,32768)==1728,"capture geometry mismatch");
 const auto& entries=record["inputs"];require(entries.kind==json::Value::Array&&entries.size()==10,"actual inputs require exactly ten C32 blocks");
 std::set<int> seen;
 for(const auto& entry:entries.array){auto c=captureEntry(entry);localFilename(c.file);
  require(c.rows==expectedRows(c.block)&&c.stride==32&&c.inputBase==0&&seen.insert(c.block).second,"duplicate/wrong-geometry C32 input");
  for(const char* name:{"reference_qkv","reference_normalized"}){const auto& ref=entry[name];localFilename(ref["file"].str());require(shaString(ref["sha256"].str()),"reference hash invalid");
   uint32_t bytes=c.rows*96u*(std::string(name)=="reference_qkv"?2u:1u);require(integer(ref["byte_length"],1,318504960)==bytes,"reference byte length mismatch");}
 }
 require(seen==std::set<int>(kBlocks.begin(),kBlocks.end()),"capture catalog missing C32 blocks");
 localFilename(record["capture_provenance"]["file"].str());require(shaString(record["capture_provenance"]["sha256"].str()),"capture provenance hash invalid");
}
void captureProvenanceMetadata(const json::Value& p,const std::string& buildHash,const std::string& captureHash){
 require(p["sourceBuildSha256"].str()==buildHash,"capture source build differs from current diagnostics");
 require(p["executableSha256"].str()==captureHash,"capture executable differs from its diagnostic build");
}
void qualifiedDevice(const vk::DeviceCapabilities& caps){require(caps.properties.vendorID==0x1002&&caps.properties.deviceID==0x7550&&std::string(caps.properties.deviceName)==kKnownDevice&&caps.driverName+"|"+caps.driverInfo+"|"+std::to_string(caps.properties.driverVersion)==kKnownDriver,"diagnostic requires qualified RX9070XT/AMD26.9.1");}
json::Value validateCatalog(const fs::path& path,const fs::path& buildDirectory={}){
 auto record=readJson(path);catalogMetadata(record);
 auto provenancePath=localFile(path,record["capture_provenance"]["file"].str());
 require(hash(read(provenancePath))==record["capture_provenance"]["sha256"].str(),"capture provenance content changed");auto p=readJson(provenancePath);
 require(p["format"].str()=="OpenNR-c32-qkv-capture-provenance-v1"&&p["modelManifestSha256"].str()==kKnownModelHash&&p["selectedShaderSha256"].str()==kAnchorAggregate&&p["baselinePolicy"].str()=="Pair-Arena-Q32-K16-off","capture provenance identity mismatch");
 trueField(p,"productionHeadExact");trueField(p,"selectiveHeadExact");require(p["driver"].str()==kKnownDriver,"capture driver outside qualified diagnostic scope");
 require(shaString(p["featuresSha256"].str())&&shaString(p["headSha256"].str())&&shaString(p["sourceBuildSha256"].str())&&shaString(p["executableSha256"].str()),"capture provenance hash incomplete");
 require(integer(p["validationErrors"],0,100000)==0,"capture had validation errors");
 if(!buildDirectory.empty()){auto build=readJson(buildDirectory/"diagnostic-build.json");auto digest=build["executables"]["amd_c32_qkv_normalize_capture.exe"].str();require(shaString(digest)&&hash(read(buildDirectory/"amd_c32_qkv_normalize_capture.exe"))==digest,"current capture executable/build binding differs");captureProvenanceMetadata(p,hash(read(buildDirectory/"diagnostic-build.json")),digest);}
 for(const auto& entry:record["inputs"].array){auto c=captureEntry(entry);auto input=read(localFile(path,c.file));require(input.size()==size_t(align64(c.rows))*c.stride&&hash(input)==c.sha256,"actual input hash/length mismatch");
  for(const char* key:{"reference_qkv","reference_normalized"}){const auto& ref=entry[key];auto data=read(localFile(path,ref["file"].str()));require(data.size()==size_t(ref["byte_length"].integer())&&hash(data)==ref["sha256"].str(),"graph reference hash/length mismatch");}}
 return record;
}
std::vector<Fixture> actualInputs(nr::Model& model,const fs::path& path){
 auto record=validateCatalog(path);require(model.manifestSha256()==kKnownModelHash,"actual input model mismatch");std::vector<Fixture> out;
 for(const auto& entry:record["inputs"].array){auto c=captureEntry(entry);auto f=fixture("actual-input-block"+std::to_string(c.block)+"-"+std::to_string(out.size()),c.rows,c.stride,c.inputBase);
  f.input=read(localFile(path,c.file));require(hash(f.input)==c.sha256,"input changed since catalog validation");
  f.inputSourceSha256=hash(f.input);f.inputBoundary="ffn_quantized";f.sourceManifestSha256=hash(read(path));f.actualInput=true;
  f.referenceQkvSha256=entry["reference_qkv"]["sha256"].str();f.referenceNormalizedSha256=entry["reference_normalized"]["sha256"].str();
  loadWeights(f,model,c.block);out.push_back(std::move(f));}return out;
}
unsigned cpuChecks(const fs::path& base){
 unsigned checks=0;auto check=[&](bool ok,const char* message){require(ok,message);++checks;};
 for(uint32_t k=0;k<32;++k)check(nr::inversePackedInputIndex(nr::packedInputIndex(k))==k,"input permutation inverse");
 // Explicit matrix scratch -> bridge mapping is a bijection, independent of fragments.
 std::set<uint32_t> bridge;
 for(uint32_t thread=0;thread<256;++thread)for(uint32_t i=thread;i<1536;i+=256){uint32_t r=i/96,c=i%96,s=(c/16)*256+r*16+c%16;check(bridge.insert(s).second,"bridge collision");check(s/256*16+s%16==c&&(s%256)/16==r,"bridge mapping");}
 check(bridge.size()==1536,"bridge missing entry");
 for(uint32_t lane=0;lane<256;++lane)for(uint32_t d:{8u,4u,2u,1u})check((lane/16)==((lane^d)/16),"shuffle escapes row");
 auto fixtures=synthetic(false);check(fixtures.size()==120,"bounded fixture count");
 for(const auto& f:fixtures){f.validate();check(f.identity().size()==64,"fixture identity");}
 // CPU source/index witnesses only. No independent full numeric oracle is claimed.
 auto mixed=fixtures[83];check(mixed.name=="mixed-Q-small-K-final-overflow","mixed index changed");
 check(num::e4m3ToF32(mixed.input[0])==32.f,"mixed input32");
 check(num::e4m3ToF32(mixed.weights[0])==.140625f&&num::e4m3ToF32(mixed.weights[32*32])==-1.5f,"mixed Q/K weight mapping");
 auto entry=json::parse("{\"block\":1,\"rows\":73,\"padded_rows\":128,\"input_stride\":32,\"input_column_base\":0,\"input_boundary\":\"ffn_quantized\",\"layout\":\"row-major-e4\",\"file\":\"private.bin\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"byte_length\":4096}");
 auto valid=captureEntry(entry);check(valid.block==1&&valid.rows==73&&valid.stride==32,"capture entry parse");
 for(auto [key,value]:std::array<std::pair<const char*,const char*>,13>{{
 {"input_boundary","\"block_input\""},{"layout","\"column-major-e4\""},{"block","5"},{"block","71"},{"rows","-1"},{"rows","73.5"},{"input_stride","31"},{"input_stride","33"},{"input_column_base","4"},{"padded_rows","73"},{"byte_length","4095"},{"file","\"\""},{"sha256","\"0\""}}}){
  auto bad=entry;bad.object[key]=json::parse(value);bool rejected=false;try{captureEntry(bad);}catch(const std::exception&){rejected=true;}check(rejected,"invalid capture boundary/shape accepted");
 }
 for(uint32_t i=0;i<65536;++i){
  uint32_t exponent=(i>>10)&31,mantissa=i&1023,sign=(i&0x8000)<<16,expected;
  if(exponent==31)expected=sign|0x7f800000|(mantissa<<13);
  else{float magnitude=std::ldexp(float(exponent?1024+mantissa:mantissa),exponent?int(exponent)-25:-24);expected=num::f32Bits(magnitude)|sign;}
  check(num::f32Bits(num::f16ToF32(uint16_t(i)))==expected,"independent half-scale expansion witness");
 }
 for(auto [file,expected]:std::array<std::pair<const char*,const char*>,4>{{
 {"baseline-shaders/amd_gemm_direct_rte_pair.spv",kPair},{"baseline-shaders/amd_window_normalize.spv",kNormalize},
 {"shaders/amd_qkv32_normalize_wave6.spv",kProduction},{"shaders/amd_qkv32_normalize_wave6_capture.spv",kCapture}}})check(hash(read(base/file))==expected,"frozen shader identity");
 for(const char* bad:{"",".","..","../input.u8","sub/input.u8","sub\\input.u8","C:input.u8","C:/input.u8","/input.u8","\\\\host\\input.u8","input.u8:stream"}){
  bool failed=false;try{localFilename(bad);}catch(const std::exception&){failed=true;}check(failed,"unsafe catalog path accepted");}
 localFilename("block-0-ffnQuantized.u8");++checks;
 auto catalog=json::parse(R"({"format":"OpenNR-c32-qkv-ffn-inputs-v1","model_manifest_sha256":"","baseline_policy":"Pair-Arena-Q32-K16-off","source_shader_sha256":"","valid_width":1707,"valid_height":960,"padded_width":1728,"capture_provenance":{"file":"capture-provenance.json","sha256":""},"inputs":[]})");
 catalog.object["model_manifest_sha256"]=json::parse(quote(kKnownModelHash));catalog.object["source_shader_sha256"]=json::parse(quote(kAnchorAggregate));catalog.object["capture_provenance"].object["sha256"]=json::parse(quote(std::string(64,'0')));
 for(int block:kBlocks){auto e=entry;e.object["block"]=json::parse(std::to_string(block));for(const char* key:{"rows","padded_rows"})e.object[key]=json::parse(std::to_string(expectedRows(block)));e.object["byte_length"]=json::parse(std::to_string(expectedRows(block)*32));
  for(const char* key:{"reference_qkv","reference_normalized"}){auto ref=json::parse("{\"file\":\"reference.bin\",\"sha256\":"+quote(std::string(64,'0'))+",\"byte_length\":"+std::to_string(expectedRows(block)*96*(std::string(key)=="reference_qkv"?2:1))+"}");e.object[key]=ref;}catalog.object["inputs"].array.push_back(e);}
 catalogMetadata(catalog);++checks;
 for(const auto& [key,value]:std::array<std::pair<const char*,const char*>,6>{{{"format","\"legacy\""},{"model_manifest_sha256","\"bad\""},{"baseline_policy","\"auto\""},{"source_shader_sha256","\"bad\""},{"valid_width","1728"},{"padded_width","1707"}}}){
  auto bad=catalog;bad.object[key]=json::parse(value);bool failed=false;try{catalogMetadata(bad);}catch(const std::exception&){failed=true;}check(failed,"unsafe capture provenance metadata accepted");}
 for(unsigned mutation=0;mutation<6;++mutation){auto bad=catalog;auto& e=bad.object["inputs"].array[0];
  if(mutation==0)bad.object["inputs"].array.pop_back();if(mutation==1)e=bad.object["inputs"].array[1];if(mutation==2)e.object["input_boundary"]=json::parse("\"block_input\"");
  if(mutation==3)e.object["file"]=json::parse("\"../input.u8\"");if(mutation==4)e.object["reference_qkv"].object["sha256"]=json::parse("\"bad\"");if(mutation==5)e.object["reference_normalized"].object["byte_length"]=json::parse("1");
  bool failed=false;try{catalogMetadata(bad);}catch(const std::exception&){failed=true;}check(failed,"unsafe/duplicate capture entry accepted");}
 auto provenance=json::parse("{\"sourceBuildSha256\":"+quote(std::string(64,'0'))+",\"executableSha256\":"+quote(std::string(64,'0'))+"}");captureProvenanceMetadata(provenance,std::string(64,'0'),std::string(64,'0'));++checks;
 for(const char* key:{"sourceBuildSha256","executableSha256"}){auto bad=provenance;bad.object[key]=json::parse(quote(std::string(64,'1')));bool failed=false;try{captureProvenanceMetadata(bad,std::string(64,'0'),std::string(64,'0'));}catch(const std::exception&){failed=true;}check(failed,"capture provenance accepts different build/executable");}
 return checks+strictProofCpuChecks();
}
struct Module{
 vk::Context& ctx;VkShaderModule shader=VK_NULL_HANDLE;vk::Pipeline pipeline{};uint32_t subgroup=0;
 Module(vk::Context& c,const fs::path& path,const char* expected,const char* name,uint32_t sg,const vk::SpecConstants& spec={}):ctx(c),subgroup(sg){
  auto data=read(path);require(hash(data)==expected&&data.size()%4==0,"module identity/size mismatch");std::vector<uint32_t> words(data.size()/4);memcpy(words.data(),data.data(),data.size());
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=data.size();ci.pCode=words.data();VK_CHECK(vkCreateShaderModule(ctx.device(),&ci,nullptr,&shader));
  pipeline=ctx.createComputePipeline(shader,spec,name,sg);
 }
 ~Module(){ctx.destroyPipeline(pipeline);if(shader)vkDestroyShaderModule(ctx.device(),shader,nullptr);}
};
struct Storage{
 vk::Context& ctx;std::vector<vk::Buffer> buffers;
 ~Storage(){for(auto& b:buffers)ctx.destroyBuffer(b);}
 vk::Buffer make(const std::vector<uint8_t>& v,const char* label){auto b=ctx.createBuffer(v.size(),false,label);buffers.push_back(b);ctx.upload(b,v.data(),v.size());return b;}
 vk::Buffer host(size_t size,const char* label){auto b=ctx.createBuffer(size,true,label);buffers.push_back(b);return b;}
};
struct GemmPush{uint32_t rows,N,Nmatrix,weightColumnOffset,inputStride,inputColumnBase,outputStride,outputColumnOffset,auxHalfOffset,batches,columnGroups,initialZeroBits;};
struct NormPush{uint32_t tokens,heads,channels,scaleWordOffset;};
struct FusionPush{uint32_t rows,inputStride,inputColumnBase,weightColumnOffset,scaleWordOffset,initialZeroBits;};
template<class T>void bind(vk::Context& ctx,Module& m,VkCommandBuffer cmd,VkDescriptorSet set,const T& push){vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m.pipeline.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,ctx.pipelineLayout(),0,1,&set,0,nullptr);vkCmdPushConstants(cmd,ctx.pipelineLayout(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);}
struct Output{std::vector<uint8_t> normalized,qkv;};
Output run(vk::Context& ctx,Module& module,Module* norm,const Fixture& f,bool capture){
 Storage s{ctx};auto input=s.make(f.input,"private QKV E4 input"),weights=s.make(f.weights,"private WQKV");
 auto aux=s.make(bytes(std::vector<uint32_t>{0,0,0,0,f.scale}),"private QKV learned scale");
 auto normalized=s.make(std::vector<uint8_t>(size_t(align64(f.rows))*96,0xa5),"private normalized canary");
 auto qkv=s.make(std::vector<uint8_t>(size_t(align64(f.rows))*96*2,0xa5),"private raw QKV canary");
 auto normalizedHost=s.host(size_t(normalized.size),"private coherent normalized readback");
 vk::Buffer qkvHost{};if(norm||capture)qkvHost=s.host(size_t(qkv.size),"private coherent raw QKV readback");
 const vk::Buffer* bindings[vk::kGenericBindings]={};bindings[0]=&input;bindings[1]=&weights;bindings[4]=&aux;bindings[5]=&normalized;if(norm||capture)bindings[2]=&qkv;
 ctx.resetDescriptorPool();auto set=ctx.allocateSet(bindings);auto cmd=ctx.beginCommands();ctx.transferBarrier(cmd);
 if(norm){
  GemmPush push{f.rows,96,96,f.weightBase,f.stride,f.inputBase,96,0,0,1,6,0};bind(ctx,module,cmd,set,push);
  uint32_t groups=(f.rows+63)/64;vkCmdDispatch(cmd,6,f.overdispatch?17:std::min(groups,65535u),f.overdispatch?2:(groups+65534)/65535);
  ctx.computeBarrier(cmd);
  const vk::Buffer* nb[vk::kGenericBindings]={};nb[1]=&qkv;nb[4]=&aux;nb[5]=&normalized;auto nset=ctx.allocateSet(nb);
  NormPush np{f.rows,1,32,4};bind(ctx,*norm,cmd,nset,np);groups=(f.rows+15)/16;vkCmdDispatch(cmd,f.overdispatch?17:std::min(groups,65535u),f.overdispatch?2:(groups+65534)/65535,1);
 }else{
  FusionPush push{f.rows,f.stride,f.inputBase,f.weightBase,4,0};bind(ctx,module,cmd,set,push);
  uint32_t groups=(f.rows+15)/16;vkCmdDispatch(cmd,f.overdispatch?17:std::min(groups,65535u),f.overdispatch?2:(groups+65534)/65535,1);
 }
 // Explicit device writes -> transfer reads, then transfer writes -> host reads.
 // Context::download alone does not provide these visibility dependencies.
 VkMemoryBarrier2 toTransfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
 toTransfer.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT;
 toTransfer.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;
 toTransfer.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
 toTransfer.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;
 VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&toTransfer;
 vkCmdPipelineBarrier2(cmd,&dependency);
 VkBufferCopy copy{0,0,normalized.size};vkCmdCopyBuffer(cmd,normalized.buffer,normalizedHost.buffer,1,&copy);
 if(norm||capture){copy.size=qkv.size;vkCmdCopyBuffer(cmd,qkv.buffer,qkvHost.buffer,1,&copy);}
 VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};toHost.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;toHost.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;toHost.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;toHost.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;dependency.pMemoryBarriers=&toHost;
 vkCmdPipelineBarrier2(cmd,&dependency);ctx.endAndSubmit(cmd,true);
 Output out;out.normalized.resize(size_t(normalized.size));memcpy(out.normalized.data(),normalizedHost.mapped,out.normalized.size());
 if(norm||capture){out.qkv.resize(size_t(qkv.size));memcpy(out.qkv.data(),qkvHost.mapped,out.qkv.size());}
 for(size_t i=size_t(f.rows)*96;i<out.normalized.size();++i)require(out.normalized[i]==0xa5,"normalized padding overwritten: "+f.name);
 for(size_t i=size_t(f.rows)*96*2;i<out.qkv.size();++i)require(out.qkv[i]==0xa5,"raw QKV padding overwritten: "+f.name);
 return out;
}
struct Environment{
 std::vector<std::pair<std::string,std::optional<std::string>>> old;
 void set(const char* key,const char* value){const char* prior=getenv(key);old.emplace_back(key,prior?std::optional<std::string>(prior):std::nullopt);require(_putenv_s(key,value)==0,"diagnostic environment selection");}
 Environment(const fs::path& p){set("DLSS5VK_PIPELINE_CACHE",p.string().c_str());
  for(const auto& [key,value]:std::array<std::pair<const char*,const char*>,14>{{
   {"DLSS5VK_AMD_KERNELS","optimized"},{"DLSS5VK_AMD_ARITHMETIC","k16"},{"DLSS5VK_AMD_GEMM","direct-rte-pair"},{"DLSS5VK_AMD_TILE_N","16"},{"DLSS5VK_AMD_STAGE_K","16"},{"DLSS5VK_AMD_WINDOW_QUERIES","32"},{"DLSS5VK_AMD_WINDOW_LAYOUT","arena-rte"},{"DLSS5VK_AMD_QKV_NORMALIZE","off"},{"DLSS5VK_AMD_FUSION","0"},{"DLSS5VK_AMD_FFN32_FUSION","0"},{"DLSS5VK_AMD_QKV32_FUSION","0"},{"DLSS5VK_AMD_EXPERT_FUSION","0"},{"DLSS5VK_AMD_BLOCK_FUSION","0"},{"DLSS5VK_AMD_HARDWARE_PUBLICATION","0"}}})set(key,value);
  set("DLSS5VK_AMD_TUNING","");}
 ~Environment(){for(auto it=old.rbegin();it!=old.rend();++it)_putenv_s(it->first.c_str(),it->second?it->second->c_str():"");}
};
struct Comparison{std::string fixture,label,leftHash,rightHash;uint64_t size,different,first;};
struct Results{
 fs::path output;std::vector<Comparison> comparisons;std::vector<Fixture> fixtures;uint32_t failures=0;uint64_t bytesCompared=0;bool saveRaw=false;unsigned fullGraphReferenceHashesVerified=0;
 void reference(const Fixture& f,const std::vector<uint8_t>& qkv,const std::vector<uint8_t>& normalized){if(!f.actualInput)return;
  require(hash(qkv)==f.referenceQkvSha256&&hash(normalized)==f.referenceNormalizedSha256,"decomposed output differs from captured full-graph reference");fullGraphReferenceHashesVerified+=2;}
 void compare(const Fixture& f,const char* label,const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){
  require(a.size()==b.size(),"comparison size");uint64_t diff=0,first=a.size();for(size_t i=0;i<a.size();++i)if(a[i]!=b[i]){++diff;first=std::min<uint64_t>(first,i);}
  bytesCompared+=a.size();comparisons.push_back({f.name,label,hash(a),hash(b),a.size(),diff,first});
  if(diff){++failures;fprintf(stderr,"FAIL %s/%s: %llu bytes first%llu (%02x/%02x)\n",f.name.c_str(),label,(unsigned long long)diff,(unsigned long long)first,a[first],b[first]);}
  if(diff||saveRaw){write(output/(f.name+"-"+label+"-left.bin"),a);write(output/(f.name+"-"+label+"-right.bin"),b);}
 }
 void execute(vk::Context& ctx,Module& pair,Module& norm,Module& production,Module& twin,const Fixture& f){
  printf("fixture %s R%u stride%u%s%s\n",f.name.c_str(),f.rows,f.stride,f.actualWeights?" actual-weights":"",f.actualInput?" actual-FFN-input":"");fflush(stdout);
  auto failedBefore=failures;
  auto a=run(ctx,pair,&norm,f,true),b=run(ctx,production,nullptr,f,false),c=run(ctx,twin,nullptr,f,true);
  reference(f,a.qkv,a.normalized);
  compare(f,"normalized-production",a.normalized,b.normalized);compare(f,"normalized-capture",a.normalized,c.normalized);compare(f,"raw-qkv-capture",a.qkv,c.qkv);
  if(failures>failedBefore){write(output/(f.name+"-input.bin"),f.input);write(output/(f.name+"-wqkv.bin"),f.weights);}
  fixtures.push_back(f);
 }
 void manifest(vk::Context& ctx,const fs::path& exe,const std::string& modelHash,bool target,bool driverInfo){
  std::ostringstream s;s<<"{\n\"format\":\"OpenNR-c32-qkv-normalization-chain-v1\",\n\"status\":"<<quote(driverInfo?"DRIVER_INFO_COLLECTED":failures?"FAIL":"PASS")
   <<",\n\"gpuExecuted\":true,\n\"operatorDispatched\":"<<(driverInfo?"false":"true")<<",\n\"contextDummyInitialization\":true,\n\"readback\":"<<(driverInfo?"false":"true")
   <<",\n\"driverInfo\":"<<(driverInfo?"true":"false")<<",\n\"pipelineCaptureFlags\":"<<(driverInfo?"true":"false")<<",\n\"instrumented\":false,\n\"timingScope\":\"none-operator-diagnostic\""
   <<",\n\"targetShapes\":"<<(target?"true":"false")<<",\n\"modelManifestSha256\":"<<quote(modelHash)<<",\n\"fullModelQualified\":false,\n\"performanceQualified\":false"
   <<",\n\"device\":"<<quote(ctx.deviceName())<<",\n\"driver\":"<<quote(ctx.capabilities().driverName+"|"+ctx.capabilities().driverInfo+"|"+std::to_string(ctx.capabilities().properties.driverVersion))
   <<",\n\"vendorId\":"<<ctx.capabilities().properties.vendorID<<",\n\"deviceId\":"<<ctx.capabilities().properties.deviceID
   <<",\n\"deviceDefaultSubgroup\":"<<ctx.capabilities().subgroupSize<<",\n\"requiredSubgroupSizes\":{\"gemm\":32,\"normalization\":0,\"production\":32,\"capture\":32}"
   <<",\n\"geometry\":{\"threads\":256,\"rowsPerGroup\":16,\"matrixActiveWaves\":6,\"heads\":1,\"K\":32,\"N\":96,\"partition\":0,\"flags\":0,\"publication\":16,\"batches\":1,\"ldsBytes\":9216}"
   <<",\n\"moduleSha256\":{\"baselinePair\":"<<quote(kPair)<<",\"baselineNormalize\":"<<quote(kNormalize)<<",\"production\":"<<quote(kProduction)<<",\"rawCapture\":"<<quote(kCapture)<<"}"
   <<",\n\"selected\":"<<kSelected<<",\n\"sourceBuildSha256\":"<<quote(hash(read(exe.parent_path()/"diagnostic-build.json")))
   <<",\n\"inputCatalogSha256\":"<<quote([&]{for(const auto& f:fixtures)if(f.actualInput)return f.sourceManifestSha256;return std::string();}())
   <<",\n\"fullGraphReferenceHashesVerified\":"<<fullGraphReferenceHashesVerified
   <<",\n\"executableSha256\":"<<quote(hash(read(exe)))<<",\n\"validationEnabled\":"<<(ctx.validationEnabled()?"true":"false")<<",\n\"validationErrors\":"<<ctx.validationErrors()
   <<",\n\"fixtures\":"<<fixtures.size()<<",\n\"checks\":"<<comparisons.size()<<",\n\"failures\":"<<failures<<",\n\"comparedBytes\":"<<bytesCompared<<",\n\"fixtureRecords\":[";
  for(size_t i=0;i<fixtures.size();++i){const auto& f=fixtures[i];if(i)s<<",";s<<"\n{\"name\":"<<quote(f.name)<<",\"identitySha256\":"<<quote(f.identity())<<",\"rows\":"<<f.rows<<",\"inputStride\":"<<f.stride<<",\"inputColumnBase\":"<<f.inputBase<<",\"weightColumnBase\":"<<f.weightBase<<",\"scaleF32Bits\":"<<f.scale<<",\"block\":"<<f.block<<",\"overdispatch\":"<<(f.overdispatch?"true":"false")<<",\"actualImportedWeights\":"<<(f.actualWeights?"true":"false")<<",\"actualFFNQuantizedInput\":"<<(f.actualInput?"true":"false")<<",\"inputBoundary\":"<<quote(f.inputBoundary)<<",\"inputSha256\":"<<quote(hash(f.input))<<",\"weightSha256\":"<<quote(hash(f.weights))<<",\"sourceCaptureManifestSha256\":"<<quote(f.sourceManifestSha256)<<"}";}
  s<<"\n],\n\"comparisons\":[";
  for(size_t i=0;i<comparisons.size();++i){const auto& c=comparisons[i];if(i)s<<",";s<<"\n{\"fixture\":"<<quote(c.fixture)<<",\"label\":"<<quote(c.label)<<",\"bytes\":"<<c.size<<",\"differentBytes\":"<<c.different<<",\"firstDifference\":"<<(c.different?std::to_string(c.first):"null")<<",\"leftSha256\":"<<quote(c.leftHash)<<",\"rightSha256\":"<<quote(c.rightHash)<<"}";}
  s<<"\n]}\n";auto text=s.str();write(output/"manifest.json",{text.begin(),text.end()});
 }
};
void selectedProof(const json::Value& record,bool decomposed=false){
 auto expected=json::parse(kSelected);if(decomposed)expected.object["qkv_normalize"]=json::parse("\"off\"");const auto& actual=record["selected"];require(actual.kind==json::Value::Object&&actual.size()==expected.size(),"diagnostic selected policy missing/extra field");
 for(const auto& [key,value]:expected.object){const auto& got=actual[key];require(got.kind==value.kind,"diagnostic policy type differs");
  require(value.kind==json::Value::String?got.str()==value.str():value.kind==json::Value::Bool?got.boolean==value.boolean:got.number==value.number,"diagnostic policy differs");}
}
void strictReportMetadata(const json::Value& r,const std::string& catalogHash,const std::string& buildHash,const std::string& executableHash){
 require(r["format"].str()=="OpenNR-c32-qkv-normalization-chain-v1"&&r["status"].str()=="PASS","strict C32 report not passed");
 trueField(r,"gpuExecuted");trueField(r,"operatorDispatched");trueField(r,"readback");selectedProof(r);
 require(r["targetShapes"].kind==json::Value::Bool&&!r["targetShapes"].boolean&&r["driverInfo"].kind==json::Value::Bool&&!r["driverInfo"].boolean,"wrong strict fixture suite");
 require(integer(r["fixtures"],0,100000)==150&&integer(r["checks"],0,100000)==450&&integer(r["failures"],0,100000)==0&&integer(r["validationErrors"],0,100000)==0&&integer(r["fullGraphReferenceHashesVerified"],0,100000)==20,"strict fixture/count/validation gate unmet");
 require(r["modelManifestSha256"].str()==kKnownModelHash&&r["inputCatalogSha256"].str()==catalogHash,"strict model/catalog differs");
 require(r["driver"].str()==kKnownDriver&&r["sourceBuildSha256"].str()==buildHash&&r["executableSha256"].str()==executableHash,"strict driver/build/probe differs");
 require(integer(r["vendorId"],0,65535)==0x1002&&integer(r["deviceId"],0,65535)==0x7550&&r["device"].str()==kKnownDevice,"strict proof was produced by another device");
 for(const auto& [name,digest]:std::array<std::pair<const char*,const char*>,4>{{{"baselinePair",kPair},{"baselineNormalize",kNormalize},{"production",kProduction},{"rawCapture",kCapture}}})require(r["moduleSha256"][name].str()==digest,"strict module differs");
 const auto& fixtures=r["fixtureRecords"];const auto& comparisons=r["comparisons"];require(fixtures.kind==json::Value::Array&&fixtures.size()==150&&comparisons.kind==json::Value::Array&&comparisons.size()==450,"strict proof records incomplete");
 std::map<std::string,uint32_t> rows;unsigned syntheticCount=0,weightsCount=0,inputCount=0;std::set<int> inputBlocks;
 for(const auto& f:fixtures.array){require(f["name"].kind==json::Value::String&&rows.emplace(f["name"].str(),integer(f["rows"],1,1658880)).second&&shaString(f["identitySha256"].str()),"strict duplicate/invalid fixture");
  require(f["actualImportedWeights"].kind==json::Value::Bool&&f["actualFFNQuantizedInput"].kind==json::Value::Bool,"strict fixture identity flags absent");
  if(f["actualFFNQuantizedInput"].boolean){++inputCount;trueField(f,"actualImportedWeights");int block=int(integer(f["block"],0,70));require(validBlock(block)&&inputBlocks.insert(block).second&&uint32_t(f["rows"].integer())==expectedRows(block)&&f["inputBoundary"].str()=="ffn_quantized"&&f["sourceCaptureManifestSha256"].str()==catalogHash,"strict actual input lineage differs");}
  else if(f["actualImportedWeights"].boolean)++weightsCount;else ++syntheticCount;}
 require(syntheticCount==120&&weightsCount==20&&inputCount==10&&inputBlocks==std::set<int>(kBlocks.begin(),kBlocks.end()),"strict fixture coverage differs");
 std::set<std::pair<std::string,std::string>> seen;uint64_t total=0;
 for(const auto& c:comparisons.array){auto name=c["fixture"].str(),label=c["label"].str();require(rows.count(name)&&(label=="normalized-production"||label=="normalized-capture"||label=="raw-qkv-capture")&&seen.emplace(name,label).second,"strict duplicate/unknown comparison");
  const uint32_t bytes=align64(rows.at(name))*96u*(label=="raw-qkv-capture"?2u:1u);require(integer(c["bytes"],1,318504960)==bytes&&integer(c["differentBytes"],0,318504960)==0&&c["firstDifference"].kind==json::Value::Null&&shaString(c["leftSha256"].str())&&c["leftSha256"].str()==c["rightSha256"].str(),"strict raw comparison failed/invalid");total+=bytes;}
 require(r["comparedBytes"].kind==json::Value::Number&&r["comparedBytes"].number==double(total),"strict byte total differs");
}
void strictReport(const json::Value& r,const fs::path& base,const fs::path& catalog){strictReportMetadata(r,hash(read(catalog)),hash(read(base/"diagnostic-build.json")),hash(read(base/"amd_c32_qkv_normalize_probe.exe")));}
unsigned strictProofCpuChecks(){
 const std::string digest(64,'0');std::ostringstream s;s<<"{\"format\":\"OpenNR-c32-qkv-normalization-chain-v1\",\"status\":\"PASS\",\"gpuExecuted\":true,\"operatorDispatched\":true,\"readback\":true,\"targetShapes\":false,\"driverInfo\":false,\"fixtures\":150,\"checks\":450,\"failures\":0,\"validationErrors\":0,\"fullGraphReferenceHashesVerified\":20,\"vendorId\":4098,\"deviceId\":30032,\"device\":"<<quote(kKnownDevice)<<",\"modelManifestSha256\":"<<quote(kKnownModelHash)<<",\"inputCatalogSha256\":"<<quote(digest)<<",\"sourceBuildSha256\":"<<quote(digest)<<",\"executableSha256\":"<<quote(digest)<<",\"driver\":"<<quote(kKnownDriver)<<",\"selected\":"<<kSelected<<",\"moduleSha256\":{\"baselinePair\":"<<quote(kPair)<<",\"baselineNormalize\":"<<quote(kNormalize)<<",\"production\":"<<quote(kProduction)<<",\"rawCapture\":"<<quote(kCapture)<<"},\"fixtureRecords\":[";
 uint64_t total=0;for(unsigned i=0;i<150;++i){bool input=i>=140,weights=i>=120;int block=input?kBlocks[i-140]:-1;uint32_t rows=input?expectedRows(block):73;if(i)s<<',';s<<"{\"name\":\"fixture"<<i<<"\",\"rows\":"<<rows<<",\"identitySha256\":"<<quote(digest)<<",\"actualImportedWeights\":"<<(weights?"true":"false")<<",\"actualFFNQuantizedInput\":"<<(input?"true":"false")<<",\"block\":"<<block<<",\"inputBoundary\":\"ffn_quantized\",\"sourceCaptureManifestSha256\":"<<quote(digest)<<"}";total+=uint64_t(align64(rows))*384;}
 s<<"],\"comparedBytes\":"<<total<<",\"comparisons\":[";for(unsigned i=0;i<150;++i)for(unsigned label=0;label<3;++label){if(i||label)s<<',';uint32_t rows=i>=140?expectedRows(kBlocks[i-140]):73;const char* name=label==0?"normalized-production":label==1?"normalized-capture":"raw-qkv-capture";s<<"{\"fixture\":\"fixture"<<i<<"\",\"label\":"<<quote(name)<<",\"bytes\":"<<align64(rows)*96*(label==2?2:1)<<",\"differentBytes\":0,\"firstDifference\":null,\"leftSha256\":"<<quote(digest)<<",\"rightSha256\":"<<quote(digest)<<"}";}s<<"]}";
 auto proof=json::parse(s.str());strictReportMetadata(proof,digest,digest,digest);unsigned checks=1;
 for(const auto& [key,value]:std::array<std::pair<const char*,const char*>,16>{{{"status","\"FAIL\""},{"fixtures","149"},{"checks","449"},{"failures","1"},{"validationErrors","1"},{"fullGraphReferenceHashesVerified","19"},{"targetShapes","true"},{"driverInfo","true"},{"gpuExecuted","false"},{"sourceBuildSha256","\"changed\""},{"executableSha256","\"changed\""},{"inputCatalogSha256","\"changed\""},{"driver","\"new-driver\""},{"vendorId","4318"},{"deviceId","30033"},{"device","\"another-device\""}}}){auto bad=proof;bad.object[key]=json::parse(value);bool rejected=false;try{strictReportMetadata(bad,digest,digest,digest);}catch(const std::exception&){rejected=true;}require(rejected,"strict proof gate accepts altered report");++checks;}
 for(unsigned mutation=0;mutation<10;++mutation){auto bad=proof;
  if(mutation==0)bad.object["selected"].object["qkv_normalize"]=json::parse("\"off\"");if(mutation==1)bad.object["selected"].object.erase("qkv_normalize");if(mutation==2)bad.object["moduleSha256"].object["production"]=json::parse("\"changed\"");
  if(mutation==3)bad.object["fixtureRecords"].array.pop_back();if(mutation==4)bad.object["comparisons"].array[0].object["differentBytes"]=json::parse("1");if(mutation==5)bad.object["comparisons"].array[0].object["rightSha256"]=json::parse(quote(std::string(64,'1')));
  if(mutation==6)bad.object["comparisons"].array[0].object["bytes"]=json::parse("1");if(mutation==7)bad.object["fixtureRecords"].array[140].object["inputBoundary"]=json::parse("\"block_input\"");if(mutation==8)bad.object["fixtureRecords"].array[140].object["block"]=json::parse("1");if(mutation==9)bad.object["comparedBytes"]=json::parse("1");
  bool rejected=false;try{strictReportMetadata(bad,digest,digest,digest);}catch(const std::exception&){rejected=true;}require(rejected,"strict proof accepts missing/wrong lineage or raw comparison");++checks;}
 return checks;
}
void strictFixtureIdentities(const json::Value& proof,const std::vector<Fixture>& fixtures){
 std::map<std::string,std::string> identities;for(const auto& f:proof["fixtureRecords"].array)identities.emplace(f["name"].str(),f["identitySha256"].str());
 require(fixtures.size()==identities.size(),"strict source fixture count differs");for(const auto& f:fixtures)require(identities.at(f.name)==f.identity(),"strict fixture input/weight/scale identity differs");
}
int main(int argc,char** argv){
 try{
  const auto base=fs::absolute(argv[0]).parent_path();
  bool execute=false,cpu=false,target=false,driver=false,saveRaw=false;fs::path output,modelPath,inputManifest;
  for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--run")execute=true;else if(a=="--cpu-check")cpu=true;else if(a=="--target-shapes")target=true;else if(a=="--driver-info")driver=true;else if(a=="--save-raw")saveRaw=true;
   else if((a=="--output"||a=="--model"||a=="--actual-inputs")&&i+1<argc){fs::path p=argv[++i];if(a=="--output")output=p;else if(a=="--model")modelPath=p;else inputManifest=p;}
   else if(a=="--help"){printf("amd_c32_qkv_normalize_probe --cpu-check | --run --output NEW_DIRECTORY [--model DIR] [--actual-inputs MANIFEST] [--target-shapes] [--save-raw]\n  --run --driver-info --output NEW_DIRECTORY collects pipeline ISA only (dummy GPU init occurs). No implicit GPU execution.\n");return 0;}
   else throw std::runtime_error("unknown/missing argument: "+a);
  }
  if(cpu){require(!execute&&!target&&!driver&&!saveRaw&&output.empty()&&modelPath.empty()&&inputManifest.empty(),"CPU mode cannot request GPU/assets/output");printf("CPU structural/fixture checks PASS:%u; no Vulkan context\n",cpuChecks(base));return 0;}
  require(execute&&!output.empty(),"GPU requires explicit --run --output NEW_DIRECTORY");require(!fs::exists(output),"preserve existing output");
  require(!driver||(!target&&!saveRaw&&modelPath.empty()&&inputManifest.empty()),"driver-info cannot infer/read model/target data");
  require(inputManifest.empty()||!modelPath.empty(),"actual inputs require supported model");
  if(!modelPath.empty())require(hash(read(modelPath/"manifest.json"))==kKnownModelHash,"supported local model manifest differs");
  if(!inputManifest.empty())validateCatalog(inputManifest,base);
  auto count=cpuChecks(base);fs::create_directories(output);Environment env(output/"pipeline-cache");
  vk::Context ctx(vk::Backend::AmdFast);const auto& caps=ctx.capabilities();
  qualifiedDevice(caps);
  require(caps.fp8Matrix16&&caps.halfPublicationRte&&caps.float32SignedZeroInfNan,"required FP8 matrix/halfRTE/F32 controls unsupported");
  require(caps.properties.limits.maxComputeWorkGroupInvocations>=256&&caps.properties.limits.maxComputeWorkGroupSize[0]>=256&&ctx.maxComputeSharedMemory()>=9216,"workgroup/LDS limits");
  require(caps.subgroupOperations&VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT,"relative subgroup shuffle unsupported");
  if(driver){require(caps.pipelineStatistics,"driver statistics unavailable");ctx.setCaptureStatistics(true);}
  vk::SpecConstants spec;for(auto [id,value]:std::array<std::pair<uint32_t,uint32_t>,7>{{{0,32},{2,0},{3,0},{10,16},{11,16},{12,16},{13,0}}})spec.add(id,value);
  Module pair(ctx,base/"baseline-shaders/amd_gemm_direct_rte_pair.spv",kPair,"baseline_pair_K32_F0",32,spec);
  Module norm(ctx,base/"baseline-shaders/amd_window_normalize.spv",kNormalize,"baseline_window_normalize",0);
  Module prod(ctx,base/"shaders/amd_qkv32_normalize_wave6.spv",kProduction,"private_qkv32_norm_production",32);
  Module capture(ctx,base/"shaders/amd_qkv32_normalize_wave6_capture.spv",kCapture,"private_qkv32_norm_raw_capture",32);
  Results result;result.output=output;result.saveRaw=saveRaw;std::string modelHash;
  if(driver){for(auto [name,m]:std::array<std::pair<const char*,Module*>,4>{{{"baseline-pair",&pair},{"baseline-normalize",&norm},{"production",&prod},{"raw-capture",&capture}}}){auto text=ctx.pipelineStatistics(m->pipeline,true);write(output/(std::string(name)+"-driver-info.txt"),{text.begin(),text.end()});}}
  else{
   for(const auto& f:synthetic(target))result.execute(ctx,pair,norm,prod,capture,f);
   if(!modelPath.empty()){nr::Model model(ctx,modelPath.string(),true);modelHash=model.manifestSha256();for(const auto& f:actualWeights(model,target))result.execute(ctx,pair,norm,prod,capture,f);if(!inputManifest.empty())for(const auto& f:actualInputs(model,inputManifest))result.execute(ctx,pair,norm,prod,capture,f);}
  }
  result.manifest(ctx,fs::absolute(argv[0]),modelHash,target,driver);
  if(driver)printf("Driver information collected; operator checks not run; CPU structural checks:%u\n",count);
  else printf("QKV32 normalization %s:%zu fixtures/%zu byte comparisons/%llu bytes/%u failures; no full-model/performance qualification\n",result.failures?"FAIL":"PASS",result.fixtures.size(),result.comparisons.size(),(unsigned long long)result.bytesCompared,result.failures);
  return result.failures||ctx.validationErrors()?1:0;
 }catch(const std::exception& e){fprintf(stderr,"ERROR:%s\n",e.what());return 1;}
}

