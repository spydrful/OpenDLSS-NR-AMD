// Standalone selective synthetic-network tensor capture. MIT licensed.
// The script compiles a transparently filtered diagnostic copy of nr_graph.cpp.
#define main c32_probe_main_not_used
#include "amd_c32_qkv_normalize_probe.cpp"
#undef main
#include "nr_graph.h"
#include "reference.h"
#include "vk_readback.h"

std::string lower(std::string text){for(char& c:text)if(c>='A'&&c<='F')c+=32;return text;}
std::vector<uint8_t> safeDownload(vk::Context& ctx,const vk::Buffer& source,VkDeviceSize size){
 require(size>0&&vk::readback::validRange(source.size,size,0),"invalid diagnostic download range");
 auto staging=ctx.createBuffer(size,true,"C32 diagnostic coherent readback");
 try{require(staging.mapped&&staging.hostVisible,"readback staging unavailable");auto cmd=ctx.beginCommands();auto before=vk::readback::deviceWritesToTransfer();
  VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.memoryBarrierCount=1;d.pMemoryBarriers=&before;vkCmdPipelineBarrier2(cmd,&d);
  VkBufferCopy copy{0,0,size};vkCmdCopyBuffer(cmd,source.buffer,staging.buffer,1,&copy);auto after=vk::readback::transferWritesToHost();d.pMemoryBarriers=&after;vkCmdPipelineBarrier2(cmd,&d);ctx.endAndSubmit(cmd,true);
  std::vector<uint8_t> data(size_t(size),0);memcpy(data.data(),staging.mapped,data.size());ctx.destroyBuffer(staging);return data;
 }catch(...){ctx.destroyBuffer(staging);throw;}
}
int main(int argc,char** argv){try{
 const auto base=fs::absolute(argv[0]).parent_path();bool cpu=false,execute=false;fs::path modelPath,featurePath,priorPath,priorReport,shaderPath,output;
 for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--cpu-check")cpu=true;else if(a=="--run")execute=true;
  else if((a=="--model"||a=="--features"||a=="--prior-head"||a=="--prior-report"||a=="--shaders"||a=="--output")&&i+1<argc){fs::path p=argv[++i];if(a=="--model")modelPath=p;else if(a=="--features")featurePath=p;else if(a=="--prior-head")priorPath=p;else if(a=="--prior-report")priorReport=p;else if(a=="--shaders")shaderPath=p;else output=p;}
  else if(a=="--help"){printf("amd_c32_qkv_normalize_capture --cpu-check | --run --model DIR --features F32 --prior-head F32 --prior-report MODELCHECK_JSON --shaders DIR --output NEW_DIRECTORY\nGenerates local synthetic-network FFN inputs/raw QKV/normalized references for ten C32 blocks; no game or timing claim.\n");return 0;}
  else throw std::runtime_error("unknown/missing argument: "+a);
 }
 if(cpu){require(!execute&&modelPath.empty()&&featurePath.empty()&&priorPath.empty()&&priorReport.empty()&&shaderPath.empty()&&output.empty(),"CPU capture mode cannot request GPU/assets/output");
  auto before=vk::readback::deviceWritesToTransfer(),after=vk::readback::transferWritesToHost();require(before.dstAccessMask==VK_ACCESS_2_TRANSFER_READ_BIT&&after.dstStageMask==VK_PIPELINE_STAGE_2_HOST_BIT&&after.dstAccessMask==VK_ACCESS_2_HOST_READ_BIT,"safe readback dependency mismatch");
  printf("CPU C32 selective capture checks PASS:%u; no Vulkan context\n",cpuChecks(base)+1);return 0;}
 require(execute&&!modelPath.empty()&&!featurePath.empty()&&!priorPath.empty()&&!priorReport.empty()&&!shaderPath.empty()&&!output.empty(),"capture requires explicit --run and all local inputs");require(!fs::exists(output),"preserve existing capture output");
 require(hash(read(modelPath/"manifest.json"))==kKnownModelHash,"supported local model differs");auto features=read(featurePath),priorHead=read(priorPath);auto prior=readJson(priorReport);
 require(prior["format"].str()=="OpenNR-local-modelcheck-v1"&&prior["backend"].str()=="amd"&&lower(prior["modelManifestSha256"].str())==kKnownModelHash,"prior modelcheck identity differs");selectedProof(prior,true);trueField(prior,"productionRepeatable");trueField(prior,"captureHeadIdentical");
 require(lower(prior["identicalFeaturesSha256"].str())==hash(features)&&prior["identity"]["shader_sha256"].str()==kAnchorAggregate,"prior feature/shader identity differs");
 const auto geometry=nr::Geometry::fromValid(1707,960);require(features.size()==size_t(geometry.fullWidth)*geometry.fullHeight*64&&priorHead.size()==size_t(geometry.fullWidth)*geometry.fullHeight*16,"capture geometry differs");
 auto build=readJson(base/"diagnostic-build.json");require(build["executables"]["amd_c32_qkv_normalize_capture.exe"].str()==hash(read(base/"amd_c32_qkv_normalize_capture.exe")),"capture executable/build binding differs");
 fs::create_directories(output);Environment env(output/"pipeline-cache");vk::Context context(vk::Backend::AmdFast);context.setCaptureStatistics(false);
 qualifiedDevice(context.capabilities());
 nr::Model model(context,modelPath.string(),true);nr::Kernels kernels(context,shaderPath.string());kernels.setSiluTable(ref::siluTable());require(kernels.shaderSha256()==kAnchorAggregate,"decomposed Pair/Arena anchor aggregate differs");
 nr::Activation input;input.rows=geometry.fullWidth*geometry.fullHeight;input.allocRows=nr::alignRows(input.rows);input.channels=16;input.format=nr::Format::F32;input.buffer=context.createBuffer(features.size(),false,"C32 generated network features");context.upload(input.buffer,features.data(),features.size());
 std::vector<uint8_t> production;
 {nr::Graph graph(context,model,kernels,geometry,{.captureBoundaries=false,.captureIntermediates=false,.fusedBlocks=false});context.resetDescriptorPool();auto cmd=context.beginCommands();context.transferBarrier(cmd);graph.record(cmd,input);context.endAndSubmit(cmd,true);production=safeDownload(context,graph.head().buffer,graph.head().validBytes());require(production==priorHead,"production head differs from prior modelcheck");}
 nr::Graph graph(context,model,kernels,geometry,{.captureBoundaries=true,.captureIntermediates=true,.fusedBlocks=false});context.resetDescriptorPool();auto cmd=context.beginCommands();context.transferBarrier(cmd);graph.record(cmd,input);context.endAndSubmit(cmd,true);auto head=safeDownload(context,graph.head().buffer,graph.head().validBytes());require(head==production,"selective capture changes production head");require(graph.boundaries().size()==30,"selective capture requires exactly thirty tensors");
 std::map<std::string,json::Value> references;
 for(const auto& [name,a]:graph.boundaries()){auto data=safeDownload(context,a->buffer,a->validBytes());std::string file=name;std::replace(file.begin(),file.end(),'/','-');file+=a->format==nr::Format::F16?".f16":".u8";write(output/file,data);
  references.emplace(name,json::parse("{\"file\":"+quote(file)+",\"sha256\":"+quote(hash(data))+",\"byte_length\":"+std::to_string(data.size())+"}"));}
 std::ostringstream provenance;provenance<<"{\"format\":\"OpenNR-c32-qkv-capture-provenance-v1\",\"gameCapture\":false,\"ordinaryPerformanceMeasured\":false,\"productionHeadExact\":true,\"selectiveHeadExact\":true,\"modelManifestSha256\":"<<quote(kKnownModelHash)<<",\"selectedShaderSha256\":"<<quote(kernels.shaderSha256())<<",\"baselinePolicy\":\"Pair-Arena-Q32-K16-off\",\"featuresSha256\":"<<quote(hash(features))<<",\"headSha256\":"<<quote(hash(head))<<",\"priorReportSha256\":"<<quote(hash(read(priorReport)))<<",\"driver\":"<<quote(kKnownDriver)<<",\"device\":"<<quote(context.deviceName())<<",\"sourceBuildSha256\":"<<quote(hash(read(base/"diagnostic-build.json")))<<",\"executableSha256\":"<<quote(hash(read(base/"amd_c32_qkv_normalize_capture.exe")))<<",\"validationErrors\":"<<context.validationErrors()<<"}\n";
 auto p=provenance.str();write(output/"capture-provenance.json",{p.begin(),p.end()});
 std::ostringstream catalog;catalog<<"{\"format\":\"OpenNR-c32-qkv-ffn-inputs-v1\",\"model_manifest_sha256\":"<<quote(kKnownModelHash)<<",\"baseline_policy\":\"Pair-Arena-Q32-K16-off\",\"source_shader_sha256\":"<<quote(kAnchorAggregate)<<",\"valid_width\":1707,\"valid_height\":960,\"padded_width\":1728,\"capture_provenance\":{\"file\":\"capture-provenance.json\",\"sha256\":"<<quote(hash(read(output/"capture-provenance.json")))<<"},\"inputs\":[";
 auto emitRef=[&](const std::string& name){const auto& r=references.at(name);catalog<<"{\"file\":"<<quote(r["file"].str())<<",\"sha256\":"<<quote(r["sha256"].str())<<",\"byte_length\":"<<r["byte_length"].integer()<<"}";};
 for(size_t i=0;i<kBlocks.size();++i){int block=kBlocks[i];std::string prefix="block-"+std::to_string(block)+"/";const auto& in=references.at(prefix+"ffnQuantized");if(i)catalog<<',';catalog<<"{\"block\":"<<block<<",\"rows\":"<<expectedRows(block)<<",\"padded_rows\":"<<expectedRows(block)<<",\"input_stride\":32,\"input_column_base\":0,\"input_boundary\":\"ffn_quantized\",\"layout\":\"row-major-e4\",\"file\":"<<quote(in["file"].str())<<",\"sha256\":"<<quote(in["sha256"].str())<<",\"byte_length\":"<<in["byte_length"].integer()<<",\"reference_qkv\":";emitRef(prefix+"qkv");catalog<<",\"reference_normalized\":";emitRef(prefix+"normalized");catalog<<'}';}
 catalog<<"]}\n";auto text=catalog.str();write(output/"qkv-inputs.json",{text.begin(),text.end()});validateCatalog(output/"qkv-inputs.json",base);context.destroyBuffer(input.buffer);
 printf("PASS thirty local generated-network tensors; production/selective heads exact; no game/quality/timing claim\n");return context.validationErrors()?1:0;
}catch(const std::exception& e){fprintf(stderr,"ERROR:%s\n",e.what());return 1;}}
