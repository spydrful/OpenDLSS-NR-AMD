// Standalone persistent-command C32 QKV normalization timing. MIT.
#define main qkv32_operator_main_not_used
#include "amd_c32_qkv_normalize_probe.cpp"
#undef main
#include <numeric>

uint32_t targetRows(int block){require(validBlock(block),"timing block is outside all10 C32 scope");return block==0||block==70?1658880u:414720u;}
struct NormalGrid{uint32_t x,y;};
NormalGrid normalGrid(uint32_t rows){
 require(rows>0&&rows<=1658880,"normalization row grid outside bounded target");
 const uint32_t groups=(rows+15u)/16u;return {std::min(groups,65535u),(groups+65534u)/65535u};
}
void validateTimingFixture(const Fixture& f){
 f.validate();require(!f.overdispatch&&f.rows==targetRows(f.block)&&f.stride==32&&f.inputBase==0&&f.weightBase==0&&f.actualWeights&&f.actualInput,
  "timing fixed to ten actual target C32 QKV inputs; full blocks0/70 and ordinary/transition1-4/66-69");
}
enum class Role{Baseline=0,Production=1,Capture=2};
const char* roleName(Role r){return r==Role::Baseline?"baseline":r==Role::Production?"candidate":"capture";}
struct Stats{
 size_t count=0;double mean=0,median=0,p95=0,p99=0,minimum=0,maximum=0,stdev=0,variation=0;
 std::string json()const{std::ostringstream s;s<<std::setprecision(17)<<"{\"samples\":"<<count<<",\"mean\":"<<mean<<",\"median\":"<<median<<",\"p95\":"<<p95<<",\"p99\":"<<p99<<",\"minimum\":"<<minimum<<",\"maximum\":"<<maximum<<",\"standardDeviation\":"<<stdev<<",\"coefficientOfVariation\":"<<variation<<"}";return s.str();}
};
double percentile(std::vector<double> v,double p){require(!v.empty(),"empty timing samples");std::sort(v.begin(),v.end());double index=(v.size()-1)*p;size_t lo=size_t(index),hi=std::min(lo+1,v.size()-1);return v[lo]+(v[hi]-v[lo])*(index-lo);}
Stats statistics(const std::vector<double>& values){
 require(!values.empty()&&std::all_of(values.begin(),values.end(),[](double x){return std::isfinite(x)&&x>0;}),"invalid GPU timing");
 Stats s;s.count=values.size();s.mean=std::accumulate(values.begin(),values.end(),0.)/values.size();s.median=percentile(values,.5);s.p95=percentile(values,.95);s.p99=percentile(values,.99);s.minimum=*std::min_element(values.begin(),values.end());s.maximum=*std::max_element(values.begin(),values.end());
 double variance=0;for(double x:values)variance+=(x-s.mean)*(x-s.mean);s.stdev=std::sqrt(variance/values.size());s.variation=s.stdev/s.mean;return s;
}
std::string samplesJson(const std::vector<double>& values){std::ostringstream s;s<<std::setprecision(17)<<"[";for(size_t i=0;i<values.size();++i){if(i)s<<",";s<<values[i];}return s.str()+"]";}
struct Persistent{
 vk::Context& ctx;Storage storage;Fixture fixture;VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
 std::array<VkCommandBuffer,3> commands{};std::array<VkQueryPool,3> queries{};uint64_t timestampMask=~uint64_t(0);
 vk::Buffer input,weights,aux,baselineQkv,baselineNorm,candidateNorm,captureQkv,captureNorm;
 std::array<vk::Buffer,5> host{};
 Persistent(vk::Context& c,Module& pair,Module& norm,Module& prod,Module& twin,const Fixture& f):ctx(c),storage{c},fixture(f){
  validateTimingFixture(f);
  uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical(),&count,nullptr);std::vector<VkQueueFamilyProperties> props(count);vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical(),&count,props.data());
  require(ctx.queueFamily()<props.size()&&props[ctx.queueFamily()].timestampValidBits>0,"queue timestamps unavailable");uint32_t bits=props[ctx.queueFamily()].timestampValidBits;if(bits<64)timestampMask=(uint64_t(1)<<bits)-1;
  input=storage.make(f.input,"persistent actual FFN input");weights=storage.make(f.weights,"persistent WQKV");aux=storage.make(bytes(std::vector<uint32_t>{0,0,0,0,f.scale}),"persistent learned scale");
  size_t n=size_t(align64(f.rows))*96,q=n*2;
  baselineQkv=storage.make(std::vector<uint8_t>(q,0xa5),"persistent baseline QKV");baselineNorm=storage.make(std::vector<uint8_t>(n,0xa5),"persistent baseline normalized");
  candidateNorm=storage.make(std::vector<uint8_t>(n,0xa5),"persistent candidate normalized");captureQkv=storage.make(std::vector<uint8_t>(q,0xa5),"persistent twin QKV");captureNorm=storage.make(std::vector<uint8_t>(n,0xa5),"persistent twin normalized");
  for(size_t i=0;i<5;++i)host[i]=storage.host(i==0||i==3?q:n,"persistent diagnostic coherent readback");
  // One descriptor-pool advance per fixture, never per measured submission.
  ctx.resetDescriptorPool();
  const vk::Buffer* b[vk::kGenericBindings]={};b[0]=&input;b[1]=&weights;b[2]=&baselineQkv;b[4]=&aux;b[5]=&baselineNorm;auto gemmSet=ctx.allocateSet(b);
  const vk::Buffer* nb[vk::kGenericBindings]={};nb[1]=&baselineQkv;nb[4]=&aux;nb[5]=&baselineNorm;auto normSet=ctx.allocateSet(nb);
  b[2]=nullptr;b[5]=&candidateNorm;auto prodSet=ctx.allocateSet(b);b[2]=&captureQkv;b[5]=&captureNorm;auto twinSet=ctx.allocateSet(b);
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=ctx.queueFamily();VK_CHECK(vkCreateCommandPool(ctx.device(),&pci,nullptr,&pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=3;VK_CHECK(vkAllocateCommandBuffers(ctx.device(),&cai,commands.data()));
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK_CHECK(vkCreateFence(ctx.device(),&fi,nullptr,&fence));
  for(unsigned r=0;r<3;++r){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;VK_CHECK(vkCreateQueryPool(ctx.device(),&qi,nullptr,&queries[r]));
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_CHECK(vkBeginCommandBuffer(commands[r],&begin));auto cmd=commands[r];
   // Shader-only dependencies cover previous reusable-buffer submissions. They
   // precede timestamps and introduce no transfer/readback in measured commands.
   ctx.computeBarrier(cmd);vkCmdResetQueryPool(cmd,queries[r],0,2);vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,queries[r],0);
   const auto grid=normalGrid(f.rows);
   if(r==0){GemmPush push{f.rows,96,96,f.weightBase,f.stride,f.inputBase,96,0,0,1,6,0};bind(ctx,pair,cmd,gemmSet,push);vkCmdDispatch(cmd,6,(f.rows+63)/64,1);ctx.computeBarrier(cmd);NormPush np{f.rows,1,32,4};bind(ctx,norm,cmd,normSet,np);vkCmdDispatch(cmd,grid.x,grid.y,1);}
   else{FusionPush push{f.rows,f.stride,f.inputBase,f.weightBase,4,0};bind(ctx,r==1?prod:twin,cmd,r==1?prodSet:twinSet,push);vkCmdDispatch(cmd,grid.x,grid.y,1);}
   vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,queries[r],1);VK_CHECK(vkEndCommandBuffer(cmd));
  }
  // Upload visibility is established once, outside warmups and timing.
  auto start=ctx.beginCommands();ctx.transferBarrier(start);ctx.endAndSubmit(start,true);
 }
 ~Persistent(){if(ctx.device())vkQueueWaitIdle(ctx.queue());if(fence)vkDestroyFence(ctx.device(),fence,nullptr);for(auto q:queries)if(q)vkDestroyQueryPool(ctx.device(),q,nullptr);if(pool)vkDestroyCommandPool(ctx.device(),pool,nullptr);}
 double submit(Role role){
  unsigned r=unsigned(role);VK_CHECK(vkResetFences(ctx.device(),1,&fence));VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&commands[r];VK_CHECK(vkQueueSubmit(ctx.queue(),1,&info,fence));VK_CHECK(vkWaitForFences(ctx.device(),1,&fence,VK_TRUE,UINT64_MAX));
  uint64_t stamps[2]={};VK_CHECK(vkGetQueryPoolResults(ctx.device(),queries[r],0,2,sizeof(stamps),stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));double ms=double((stamps[1]-stamps[0])&timestampMask)*ctx.timestampPeriodNs()/1000000.;require(std::isfinite(ms)&&ms>0,"invalid timestamp delta");return ms;
 }
 std::array<std::vector<uint8_t>,5> readOutputs(){
  // All three persistent paths execute with the same fixed inputs outside timing.
  submit(Role::Baseline);submit(Role::Production);submit(Role::Capture);
  const std::array<vk::Buffer,5> gpu={baselineQkv,baselineNorm,candidateNorm,captureQkv,captureNorm};
  auto cmd=ctx.beginCommands();VkMemoryBarrier2 transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};transfer.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT;transfer.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;transfer.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;transfer.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&transfer;vkCmdPipelineBarrier2(cmd,&dependency);
  for(unsigned i=0;i<5;++i){VkBufferCopy copy{0,0,gpu[i].size};vkCmdCopyBuffer(cmd,gpu[i].buffer,host[i].buffer,1,&copy);}
  VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};toHost.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;toHost.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;toHost.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;toHost.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;dependency.pMemoryBarriers=&toHost;vkCmdPipelineBarrier2(cmd,&dependency);ctx.endAndSubmit(cmd,true);
  std::array<std::vector<uint8_t>,5> out;for(unsigned i=0;i<5;++i){out[i].resize(size_t(gpu[i].size));memcpy(out[i].data(),host[i].mapped,out[i].size());size_t valid=size_t(fixture.rows)*96*(i==0||i==3?2:1);for(size_t offset=valid;offset<out[i].size();++offset)require(out[i][offset]==0xa5,"persistent padding canary overwritten");}
  // Re-establish transfer/compute ordering once after each diagnostic phase,
  // outside all measured commands and warmup runs.
  auto resume=ctx.beginCommands();ctx.transferBarrier(resume);ctx.endAndSubmit(resume,true);return out;
 }
};
struct Run{std::string fixture;unsigned pair;Role role;std::vector<double> samples;};
void verify(Persistent& p,Results& result,const char* phase){
 auto a=p.readOutputs();auto f=p.fixture;f.name+="-"+std::string(phase);
 result.reference(f,a[0],a[1]);
 result.compare(f,"normalized-production",a[1],a[2]);result.compare(f,"normalized-capture",a[1],a[4]);result.compare(f,"raw-qkv-capture",a[0],a[3]);result.fixtures.push_back(std::move(f));
 require(result.failures==0,"pre/post output mismatch; timing not qualified");
}
unsigned timingCpuChecks(const fs::path& base){
 unsigned checks=cpuChecks(base);auto check=[&](bool ok,const char* message){require(ok,message);++checks;};
 auto s=statistics({1,2,3,4,5});check(s.median==3&&s.p95==4.8&&s.p99==4.96&&s.mean==3,"percentile/stats convention");
 check(percentile({2,4},.5)==3,"even sample median");
 check(sizeof(GemmPush)==48&&sizeof(NormPush)==16&&sizeof(FusionPush)==24,"push layouts");
 for(unsigned pair=1;pair<=3;++pair){auto order=pair%2?std::array<Role,2>{Role::Baseline,Role::Production}:std::array<Role,2>{Role::Production,Role::Baseline};check(order[0]!=order[1],"interleaved pair order");}
 for(int block:kBlocks){
  auto expected=block==0?std::pair<uint32_t,uint32_t>{9312,20576}:block==66?std::pair<uint32_t,uint32_t>{10400,21664}:block==70?std::pair<uint32_t,uint32_t>{8400,19664}:std::pair<uint32_t,uint32_t>{8288,19552};
  check(blockOffsets(block)==expected,"all10 model offsets");
  check(targetRows(block)==(block==0||block==70?1658880u:414720u),"all10 target row geometry");
 }
 for(auto [rows,x,y]:std::array<std::array<uint32_t,3>,5>{{{1,1,1},{414720,25920,1},{1048560,65535,1},{1048561,65535,2},{1658880,65535,2}}}){
  auto grid=normalGrid(rows);check(grid.x==x&&grid.y==y,"clipped normalization grid");
  check((uint64_t(grid.y-1)*65535u+grid.x)>=((rows+15u)/16u),"grid covers actual target rows");
 }
 for(uint32_t invalid:{0u,1658881u}){bool rejected=false;try{normalGrid(invalid);}catch(const std::exception&){rejected=true;}check(rejected,"invalid normalization geometry accepted");}
 for(int block:{5,65,71,-1}){bool rejected=false;try{targetRows(block);}catch(const std::exception&){rejected=true;}check(rejected,"invalid all10 timing block accepted");}
 return checks;
}
int main(int argc,char** argv){
 try{
  const auto base=fs::absolute(argv[0]).parent_path();bool cpu=false,execute=false;fs::path output,modelPath,catalog,strict;
  for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--cpu-check")cpu=true;else if(a=="--run")execute=true;
   else if((a=="--output"||a=="--model"||a=="--actual-inputs"||a=="--strict-report")&&i+1<argc){fs::path p=argv[++i];if(a=="--output")output=p;else if(a=="--model")modelPath=p;else if(a=="--strict-report")strict=p;else catalog=p;}
   else if(a=="--help"){printf("amd_c32_qkv_normalize_timing --cpu-check | --run --model DIR --actual-inputs LOCAL_ALL10_CATALOG --strict-report MATCHED_PASS_REPORT --output NEW_DIRECTORY\nFixed all10 C32 target captures:0/70 R1658880;1-4/66-69 R414720. Catalog/model/shader/report identities bound;5 warmups per run,3 interleaved pairs,30 measured frames per role/run. No GPU without explicit --run.\n");return 0;}
   else throw std::runtime_error("unknown/missing argument: "+a);
  }
  if(cpu){require(!execute&&output.empty()&&modelPath.empty()&&catalog.empty()&&strict.empty(),"CPU timing mode cannot request GPU/assets/output");printf("CPU persistent timing checks PASS:%u; no Vulkan context\n",timingCpuChecks(base));return 0;}
  require(execute&&!output.empty()&&!modelPath.empty()&&!catalog.empty()&&!strict.empty(),"GPU timing requires --run --model --actual-inputs --strict-report --output NEW_DIRECTORY");
  require(!fs::exists(output),"preserve existing timing evidence");
  require(hash(read(modelPath/"manifest.json"))==kKnownModelHash,"supported local model identity differs");validateCatalog(catalog,base);auto proof=readJson(strict);strictReport(proof,base,catalog);
  auto count=timingCpuChecks(base);fs::create_directories(output/"verification");Environment env(output/"pipeline-cache");vk::Context ctx(vk::Backend::AmdFast);ctx.setCaptureStatistics(false);const auto& caps=ctx.capabilities();
  qualifiedDevice(caps);
  require(caps.fp8Matrix16&&caps.halfPublicationRte&&caps.float32SignedZeroInfNan,"matrix/float controls unsupported");require(caps.properties.limits.maxComputeWorkGroupInvocations>=256&&caps.properties.limits.maxComputeWorkGroupSize[0]>=256&&ctx.maxComputeSharedMemory()>=9216,"workgroup/LDS unsupported");require(caps.subgroupOperations&VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT,"relative shuffle unsupported");
  vk::SpecConstants spec;for(auto [id,value]:std::array<std::pair<uint32_t,uint32_t>,7>{{{0,32},{2,0},{3,0},{10,16},{11,16},{12,16},{13,0}}})spec.add(id,value);
  Module pair(ctx,base/"baseline-shaders/amd_gemm_direct_rte_pair.spv",kPair,"timing_pair_K32_F0",32,spec),norm(ctx,base/"baseline-shaders/amd_window_normalize.spv",kNormalize,"timing_original_normalize",0),prod(ctx,base/"shaders/amd_qkv32_normalize_wave6.spv",kProduction,"timing_qkv32_norm",32),twin(ctx,base/"shaders/amd_qkv32_normalize_wave6_capture.spv",kCapture,"outside_timing_raw_capture",32);
  nr::Model model(ctx,modelPath.string(),true);require(model.manifestSha256()==kKnownModelHash,"supported model identity differs");auto fixtures=actualInputs(model,catalog);require(fixtures.size()==10,"timing requires exactly ten C32 target captures");std::set<int> blocks;for(const auto& f:fixtures){validateTimingFixture(f);require(blocks.insert(f.block).second,"duplicate timing block");}require(blocks==std::set<int>(kBlocks.begin(),kBlocks.end()),"timing catalog missing C32 blocks");
  auto expected=synthetic(false);auto weights=actualWeights(model,false);expected.insert(expected.end(),weights.begin(),weights.end());expected.insert(expected.end(),fixtures.begin(),fixtures.end());strictFixtureIdentities(proof,expected);expected.clear();expected.shrink_to_fit();
  Results result;result.output=output/"verification";std::vector<Run> runs;
  for(const auto& f:fixtures){
   Persistent persistent(ctx,pair,norm,prod,twin,f);verify(persistent,result,"pre");
   for(unsigned p=1;p<=3;++p){const auto order=p%2?std::array<Role,2>{Role::Baseline,Role::Production}:std::array<Role,2>{Role::Production,Role::Baseline};
    for(Role role:order){for(unsigned warmup=0;warmup<5;++warmup)persistent.submit(role);Run run{f.name,p,role,{}};for(unsigned frame=0;frame<30;++frame)run.samples.push_back(persistent.submit(role));auto stats=statistics(run.samples);printf("%s pair%u %s median%.6fms p95%.6fms\n",f.name.c_str(),p,roleName(role),stats.median,stats.p95);fflush(stdout);runs.push_back(std::move(run));}
   }
   verify(persistent,result,"post");
  }
  result.manifest(ctx,fs::absolute(argv[0]),model.manifestSha256(),true,false);
  std::ostringstream report;report<<std::setprecision(17)<<"{\n\"format\":\"OpenNR-c32-qkv-normalization-operator-timing-v1\",\n\"status\":\"COMPLETE\""
   <<",\n\"gpuExecuted\":true,\n\"scope\":\"QKV GEMM plus window normalization operator only; no full network/bridge/game\""
   <<",\n\"rowsPerFixture\":null,\n\"rowGeometry\":\"blocks0/70 R1658880;blocks1-4/66-69 R414720\",\n\"normalizationGrid\":\"x=min(ceil(rows/16),65535),y=ceil(ceil(rows/16)/65535)\",\n\"heads\":1,\n\"K\":32,\n\"N\":96,\n\"publication\":16,\n\"flags\":0,\n\"partition\":0,\n\"batches\":1"
   <<",\n\"warmupPerRun\":5,\n\"pairs\":3,\n\"measuredFramesPerRun\":30,\n\"samplesPerRolePerFixture\":90"
   <<",\n\"persistentCommandBuffers\":true,\n\"persistentDescriptorSets\":true,\n\"persistentBuffers\":true,\n\"vkFenceWaitBetweenSubmissions\":true"
   <<",\n\"readbackDuringTiming\":false,\n\"captureTwinDuringTiming\":false,\n\"pipelineStatisticsCapture\":false,\n\"dispatchInstrumentation\":false"
   <<",\n\"timestampScope\":\"compute timestamp around one fused dispatch or two decomposed dispatches including their compute barrier; initial buffer-ordering barrier excluded\""
   <<",\n\"verificationPhases\":[\"pre\",\"post\"],\n\"verificationChecks\":"<<result.comparisons.size()<<",\n\"verificationFailures\":"<<result.failures
   <<",\n\"modelManifestSha256\":"<<quote(model.manifestSha256())<<",\n\"inputCatalogSha256\":"<<quote(hash(read(catalog)))
   <<",\n\"strictReportSha256\":"<<quote(hash(read(strict)))<<",\n\"sourceBuildSha256\":"<<quote(hash(read(base/"diagnostic-build.json")))<<",\n\"selected\":"<<kSelected
   <<",\n\"driver\":"<<quote(caps.driverName+"|"+caps.driverInfo+"|"+std::to_string(caps.properties.driverVersion))<<",\n\"device\":"<<quote(ctx.deviceName())
   <<",\n\"vendorId\":"<<caps.properties.vendorID<<",\n\"deviceId\":"<<caps.properties.deviceID
   <<",\n\"moduleSha256\":{\"baselinePair\":"<<quote(kPair)<<",\"baselineNormalize\":"<<quote(kNormalize)<<",\"production\":"<<quote(kProduction)<<",\"rawCapture\":"<<quote(kCapture)<<"}"
   <<",\n\"executableSha256\":"<<quote(hash(read(fs::absolute(argv[0]))))<<",\n\"cpuChecks\":"<<count<<",\n\"validationErrors\":"<<ctx.validationErrors()<<",\n\"wholeModelQualified\":false,\n\"defaultPromotionQualified\":false,\n\"runs\":[";
  for(size_t i=0;i<runs.size();++i){const auto& r=runs[i];if(i)report<<",";report<<"\n{\"fixture\":"<<quote(r.fixture)<<",\"pair\":"<<r.pair<<",\"role\":"<<quote(roleName(r.role))<<",\"stats\":"<<statistics(r.samples).json()<<",\"samplesMs\":"<<samplesJson(r.samples)<<"}";}
  report<<"\n],\n\"pooledOperators\":[";
  for(size_t i=0;i<fixtures.size();++i){const auto& f=fixtures[i];std::vector<double> b,c;for(const auto& r:runs)if(r.fixture==f.name){auto& out=r.role==Role::Baseline?b:c;out.insert(out.end(),r.samples.begin(),r.samples.end());}require(b.size()==90&&c.size()==90,"missing timing samples");auto bs=statistics(b),cs=statistics(c);auto grid=normalGrid(f.rows);auto offsets=blockOffsets(f.block);if(i)report<<",";report<<"\n{\"fixture\":"<<quote(f.name)<<",\"block\":"<<f.block<<",\"rows\":"<<f.rows<<",\"normalizationGrid\":["<<grid.x<<","<<grid.y<<",1],\"qkvTensorOffset\":"<<offsets.first<<",\"scaleTensorOffset\":"<<offsets.second<<",\"inputSha256\":"<<quote(hash(f.input))<<",\"weightSha256\":"<<quote(hash(f.weights))<<",\"baseline\":"<<bs.json()<<",\"candidate\":"<<cs.json()<<",\"medianImprovementFraction\":"<<1-cs.median/bs.median<<",\"p95Ratio\":"<<cs.p95/bs.p95<<",\"fivePercentOperatorImprovement\":"<<(cs.median<=bs.median*.95?"true":"false")<<"}";}
  report<<"\n]}\n";auto text=report.str();write(output/"timing.json",{text.begin(),text.end()});printf("Persistent operator timing complete:ten fixtures;60 pre/post byte checks; no whole-model/default/game qualification\n");return ctx.validationErrors()?1:0;
 }catch(const std::exception& e){fprintf(stderr,"ERROR:%s\n",e.what());return 1;}
}

