// Native D3D12/Vulkan bridge for this fork's nr::Graph.
// GPL-3.0-or-later: compatibility declarations and submission integration are derived from
// MatheusFerreiraS/neural-amd-opti (557bb855). The original MIT neural core retains its license.
#define OPEN_NR_RUNTIME_EXPORTS
#define LMXXF_NR_RUNTIME_EXPORTS
#define MOCHIZUKI_NR_RUNTIME_EXPORTS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <memory>
#include <sstream>
#include "open_nr_api.h"
#include "compat/MochizukiNrControls.h"
#include "nr_graph.h"
#include "numeric.h"
#include "sha256.h"
#include "shader_identity.h"
#include "capture_request.h"
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint32_t kRuntimeFrameSlots = 8;
thread_local std::string errorText;
void check(HRESULT r,const char* text){if(FAILED(r))throw std::runtime_error(std::string(text)+" HRESULT="+std::to_string(uint32_t(r)));}
bool sameDevice(ID3D12Device* a,ID3D12Device* b){ComPtr<IUnknown> x,y;return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x.Get()==y.Get();}
bool sameQueue(ID3D12CommandQueue* a,ID3D12CommandQueue* b){ComPtr<IUnknown> x,y;return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x.Get()==y.Get();}
template<class T> bool deviceMatches(T* object,ID3D12Device* device){ComPtr<ID3D12Device> actual;return object&&SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&actual)))&&sameDevice(device,actual.Get());}
template<class F> int32_t guarded(F f){try{f();errorText.clear();return LMXXF_NR_OK;}catch(const std::exception& e){errorText=e.what();return LMXXF_NR_FAILED;}catch(...){errorText="unexpected runtime exception";return LMXXF_NR_FAILED;}}
void textOut(char* out,uint32_t n,const std::string& text){if(out&&n){size_t k=std::min<size_t>(n-1,text.size());memcpy(out,text.data(),k);out[k]=0;}}
float sane(float v,float lo,float hi,float fallback){return std::isfinite(v)?std::clamp(v,lo,hi):fallback;}
std::string fileHash(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("cannot hash capture asset");std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});return sha256Hex(bytes.data(),bytes.size());}
std::string captureIdentityHash(std::string hash){for(char& c:hash)if(c>='a'&&c<='f')c-=('a'-'A');return hash;}
std::string captureShaderHash(const shader_identity::Source& source){return captureIdentityHash(source.sha256);}
vk::Pipeline capturedShaderPipeline(vk::Context& context,const shader_identity::Source& source,const char* label){
  // The source snapshot supplies both the module bytes and capture identity.
  // Reopening its filename after loading would allow a replacement to relabel
  // a live pipeline. A shader module may be destroyed once pipeline creation
  // completes; the pipeline keeps its compiled code independently.
  struct Module{VkDevice device;VkShaderModule handle=VK_NULL_HANDLE;~Module(){if(handle)vkDestroyShaderModule(device,handle,nullptr);}} module{context.device()};
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=source.words.size()*sizeof(uint32_t);info.pCode=source.words.data();
  VK_CHECK(vkCreateShaderModule(module.device,&info,nullptr,&module.handle));
  vk::SpecConstants noSpec;return context.createComputePipeline(module.handle,noSpec,label,0);
}
std::string captureQuote(const std::string& text){std::ostringstream out;out<<'"';for(unsigned char c:text){if(c=='"'||c=='\\')out<<'\\'<<char(c);else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;else out<<char(c);}out<<'"';return out.str();}
uint32_t memoryType(vk::Context& c,uint32_t bits){VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(c.physical(),&p);for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return i;throw std::runtime_error("no shared device-local memory type");}
struct SharedBuffer {
  vk::Context* context=nullptr;ComPtr<ID3D12Resource> d3d;HANDLE handle=nullptr;vk::Buffer buffer;
  ~SharedBuffer(){if(context){context->destroyBuffer(buffer);}if(handle)CloseHandle(handle);}
  void create(ID3D12Device* device,vk::Context& c,uint64_t bytes){
    context=&c;bytes=std::max<uint64_t>((bytes+65535)&~uint64_t(65535),16ull<<20);
    VkPhysicalDeviceExternalBufferInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    query.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    query.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkExternalBufferProperties supported{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};vkGetPhysicalDeviceExternalBufferProperties(c.physical(),&query,&supported);
    if(!(supported.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))throw std::runtime_error("[unsupported] D3D12 shared storage buffers cannot be imported");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&d3d)),"shared buffer");
    check(device->CreateSharedHandle(d3d.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"shared buffer handle");
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};external.handleTypes=query.handleType;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,&external};bi.size=bytes;bi.usage=query.usage;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(c.device(),&bi,nullptr,&buffer.buffer));buffer.size=bytes;buffer.label="D3D12 shared buffer";
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(c.device(),buffer.buffer,&req);
    VkMemoryWin32HandlePropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};VK_CHECK(vkGetMemoryWin32HandlePropertiesKHR(c.device(),query.handleType,handle,&props));
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.buffer=buffer.buffer;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,&dedicated};imp.handleType=query.handleType;imp.handle=handle;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&imp};alloc.allocationSize=std::max<uint64_t>(req.size,device->GetResourceAllocationInfo(0,1,&rd).SizeInBytes);alloc.memoryTypeIndex=memoryType(c,req.memoryTypeBits&props.memoryTypeBits);
    VK_CHECK(vkAllocateMemory(c.device(),&alloc,nullptr,&buffer.memory));VK_CHECK(vkBindBufferMemory(c.device(),buffer.buffer,buffer.memory,0));
  }
};
struct SharedFence {
  vk::Context* context=nullptr;ComPtr<ID3D12Fence> fence;HANDLE handle=nullptr;VkSemaphore semaphore=VK_NULL_HANDLE;
  ~SharedFence(){if(context&&semaphore)vkDestroySemaphore(context->device(),semaphore,nullptr);if(handle)CloseHandle(handle);}
  void create(ID3D12Device* d,vk::Context& c){
    context=&c;VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,&type};q.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties p{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};vkGetPhysicalDeviceExternalSemaphoreProperties(c.physical(),&q,&p);
    if(!(p.externalSemaphoreFeatures&VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))throw std::runtime_error("[unsupported] D3D12 fence cannot be imported as timeline semaphore");
    check(d->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"shared fence");check(d->CreateSharedHandle(fence.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"fence handle");
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,&type};VK_CHECK(vkCreateSemaphore(c.device(),&si,nullptr,&semaphore));
    VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};imp.semaphore=semaphore;imp.handleType=q.handleType;imp.handle=handle;VK_CHECK(vkImportSemaphoreWin32HandleKHR(c.device(),&imp));
  }
};
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){if(before==after)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);}
struct PackParams {uint32_t width,height,colorX,colorY,motionWidth,motionHeight,motionX,motionY;float motionScaleX,motionScaleY,preExposure,exposureScale,jitterDeltaX,jitterDeltaY;uint32_t temporal,exposureAvailable;};
struct FrameParams {uint32_t fullWidth,fullHeight,width,height,seed,historyValid,autoMask,style;float tone,structure,skin,paperWhite,intensity,blendScale,historyStrength,enabled,colorStrength,maxRatio;};
static_assert(sizeof(FrameParams)==72&&sizeof(PackParams)==64);
struct FrameCapture {
  vk::Context* context=nullptr;std::array<vk::Buffer,5> data{};bool complete=true;
  std::string sequenceId;uint32_t ordinal=0,requested=0;uint64_t historyFrame=0,historySubmission=0,submittedTick=0,submittedFileTime=0;
  ~FrameCapture(){if(context)for(auto& b:data)context->destroyBuffer(b);}
  void allocate(vk::Context& c,uint32_t index,uint64_t bytes){context=&c;data[index]=c.createBuffer(bytes,true,"bounded diagnostic capture");}
  void copy(VkCommandBuffer commands,uint32_t index,const vk::Buffer& source){VkBufferCopy region{0,0,data[index].size};vkCmdCopyBuffer(commands,source.buffer,data[index].buffer,1,&region);}
};
struct PackPipelines {
  ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pack,unpack;
  void create(ID3D12Device* d,const std::filesystem::path& path){
    D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,3};
    D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants={0,0,16};p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[1].DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC desc{2,p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,errors;
    check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"pack root serialization");check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"pack root signature");
    for(unsigned i=0;i<2;i++){D3D_SHADER_MACRO macros[]={{"PACK_INPUT",i==0?"1":nullptr},{nullptr,nullptr}};
      blob.Reset();errors.Reset();HRESULT hr=D3DCompileFromFile(path.c_str(),i==0?macros:nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&blob,&errors);
      if(FAILED(hr))throw std::runtime_error(errors?std::string((char*)errors->GetBufferPointer(),errors->GetBufferSize()):"bridge HLSL compilation failed");
      D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={blob->GetBufferPointer(),blob->GetBufferSize()};check(d->CreateComputePipelineState(&ps,IID_PPV_ARGS(i==0?&pack:&unpack)),"bridge pipeline");
    }
  }
};
struct Job {
  std::unique_ptr<FrameCapture> capture;
  std::unique_ptr<SharedBuffer> input,output;ComPtr<ID3D12Resource> texture,color,motion,exposure;ComPtr<ID3D12DescriptorHeap> heaps[2];
  ComPtr<ID3D12QueryHeap> bridgeQueries;ComPtr<ID3D12Resource> bridgeReadback;
  ComPtr<ID3D12CommandAllocator> recoveryAllocator;ComPtr<ID3D12GraphicsCommandList> recoveryList;
  LmxxfNrFrameInfo frame{};OpenNrFrameMetadata metadata{};MochizukiNrControls controls{};uint64_t value=0,consumerValue=0,packedPredecessor=0;uint32_t state=LMXXF_NR_JOB_NONE;
  bool packedConsecutive=false;
  VkDevice device=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer commands=VK_NULL_HANDLE;VkQueryPool timestamps=VK_NULL_HANDLE;bool retired=false,vulkanSubmitted=false,initialized=false,outputsRecorded=false,recovered=false;PackParams pack{};FrameParams params{};ComPtr<ID3D12CommandQueue> actualQueue;
  ~Job(){if(device&&timestamps)vkDestroyQueryPool(device,timestamps,nullptr);if(device&&pool)vkDestroyCommandPool(device,pool,nullptr);}
};
struct Session {
  ComPtr<ID3D12Device> d3d;ComPtr<ID3D12CommandQueue> queue;std::filesystem::path root;
  std::unique_ptr<vk::Context> context;std::unique_ptr<SharedFence> produced,finished;ComPtr<ID3D12Fence> consumed;HANDLE event=nullptr;
  PackPipelines packer;std::unique_ptr<nr::Model> model;std::unique_ptr<nr::Kernels> kernels;std::unique_ptr<nr::Graph> graph;
  nr::Activation* features=nullptr;nr::Geometry geometry{};vk::Buffer histories[2];std::array<std::unique_ptr<Job>,kRuntimeFrameSlots> jobs;
  vk::Pipeline preprocess,composite;std::mutex mutex;uint64_t counter=0,lastFrame=0,frames=0,consumerCounter=0,lastSubmittedValue=0;uint32_t width=0,height=0,parity=0;bool historyValid=false,reset=true;
  float lastJitterX=0,lastJitterY=0,blendScale=1;std::vector<double> timings;OpenNrFrameMetadata nextMetadata{};MochizukiNrControls controls{};bool prepared=false,failed=false,controlsValid=true;uint64_t submittedFrames=0,bypassedFrames=0;OpenNrTimings lastTimings{};
  uint64_t lastPreparedValue=0,lastPreparedFrame=0;float lastPreparedJitterX=0,lastPreparedJitterY=0;
  ComPtr<IDXGIAdapter3> memoryAdapter;std::ofstream timingTrace;double sampledVramMiB=-1;uint64_t traceRows=0;
  nr::capture::Request captureRequest;uint64_t captureSequenceCounter=0;std::string captureSequenceId,modelHash,preprocessHash,compositeHash;
  void invalidateTemporal(){reset=true;lastPreparedValue=lastPreparedFrame=0;lastPreparedJitterX=lastPreparedJitterY=0;}
  ~Session(){if(context){for(auto& j:jobs)j.reset();graph.reset();kernels.reset();model.reset();for(auto& h:histories)context->destroyBuffer(h);context->destroyPipeline(preprocess);context->destroyPipeline(composite);finished.reset();produced.reset();}if(event)CloseHandle(event);}
  void waitConsumer(uint64_t value){if(!value||consumed->GetCompletedValue()>=value)return;check(consumed->SetEventOnCompletion(value,event),"consumer completion");if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("consumer drain timed out; session must remain alive");}
  void drain(){for(auto& j:jobs)if(j&&j->state!=LMXXF_NR_JOB_NONE&&!j->retired)throw std::runtime_error("cannot drain an unretired recorded continuation");waitConsumer(consumerCounter);if(context)context->waitIdle();for(auto& j:jobs)if(j)reclaim(*j);if(timingTrace)timingTrace.flush();}
  void traceCompleted(const OpenNrTimings& t){
    if(!timingTrace)return;
    if(memoryAdapter&&(traceRows%60==0)){DXGI_QUERY_VIDEO_MEMORY_INFO m{};if(SUCCEEDED(memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&m)))sampledVramMiB=double(m.CurrentUsage)/(1024*1024);}
    timingTrace << t.frame_id << ',' << t.pack_ms << ',' << t.preprocess_ms << ',' << t.inference_ms << ',' << t.composite_ms << ',' << t.unpack_ms << ',' << t.nr_bridge_ms << ',' << sampledVramMiB << ',' << t.allocated_neural_bytes << ',' << t.submitted_frames << ',' << t.bypassed_frames << '\n';
    if(++traceRows%60==0)timingTrace.flush();
  }
  void publishCapture(Job& j){
    if(!j.capture)return;
    // A requested diagnostic snapshot is separate from normal submission and
    // performance runs. All source copies were recorded before history reuse.
    try{
      if(!j.capture->complete)throw std::runtime_error("capture readback allocation incomplete");
      const auto parent=root/"captures"/j.capture->sequenceId;std::filesystem::create_directories(parent);
      const auto name="frame-"+std::to_string(j.frame.frame_id)+"-"+std::to_string(j.value)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
      const auto staged=parent/("."+name+"-staging"),destination=parent/name;
      if(std::filesystem::exists(destination)||!std::filesystem::create_directory(staged))throw std::runtime_error("capture destination already exists");
      const char* names[]={"source-packed.f32","features.f32","previous-history.f32","head.f32","scene-linear-rgba.f32"};
      auto write=[&](const std::filesystem::path& path,const void* bytes,size_t n){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write((const char*)bytes,n);file.close();if(!file)throw std::runtime_error("capture write failed");};
      for(uint32_t i=0;i<5;i++)write(staged/names[i],j.capture->data[i].mapped,size_t(j.capture->data[i].size));
      write(staged/"controls.bin",&j.params,sizeof(j.params));
      write(staged/"pack-controls.bin",&j.pack,sizeof(j.pack));
      std::ofstream manifest(staged/"manifest.json");
      wchar_t processPath[32768]{};GetModuleFileNameW(nullptr,processPath,32768);
      const auto processName=std::filesystem::path(processPath).filename().wstring();
      const bool gameCapture=CompareStringOrdinal(processName.c_str(),-1,L"Cyberpunk2077.exe",-1,TRUE)==CSTR_EQUAL;
      const auto& captured=*j.capture;const auto& options=kernels->amdPolicy();const auto& capabilities=context->capabilities();
      float gpuExposure=0;memcpy(&gpuExposure,(const uint8_t*)captured.data[0].mapped+uint64_t(width)*height*32,4);if(!std::isfinite(gpuExposure)||gpuExposure<=0)throw std::runtime_error("capture GPU exposure is invalid");
      manifest << std::setprecision(9) << "{\n  \"format\": \"OpenNR-game-capture-v1\",\n  \"gameCapture\": " << (gameCapture?"true":"false") << ",\n  \"performanceRepresentative\": false,\n  \"outputPublication\": \"Vulkan f32 scene result before D3D12 RGBA16F conversion\",\n  \"backend\": \"amd\",\n  \"arithmetic\": \"" << options.arithmeticName() << "\",\n  \"kernelMode\": \"" << kernels->selectedKernelMode() << "\",\n  \"tileN\": " << options.tileN << ",\n  \"stageK\": " << options.stageK << ",\n  \"windowQueries\": " << options.windowQueries
        << ",\n  \"fusion\": " << (options.fusion?"true":"false") << ",\n  \"ffn32Fusion\": " << (options.ffn32Enabled()?"true":"false") << ",\n  \"qkv32Fusion\": " << (options.qkv32Enabled()?"true":"false")
        << ",\n  \"gemm\": \"" << options.gemmName() << '"'
        << ",\n  \"windowLayout\": \"" << options.windowLayoutName() << '"'
        << ",\n  \"expertFusion\": " << (options.expertFusion?"true":"false") << ",\n  \"blockFusion\": " << (options.blockFusion?"true":"false") << ",\n  \"hardwarePublication\": " << (options.hardwarePublication?"true":"false")
        << ",\n  \"width\": " << width << ",\n  \"height\": " << height << ",\n  \"fullWidth\": " << geometry.fullWidth << ",\n  \"fullHeight\": " << geometry.fullHeight << ",\n  \"frame_id\": " << j.frame.frame_id << ",\n  \"session_id\": " << j.frame.session_id << ",\n  \"submission_id\": " << j.value << ",\n  \"list_generation\": " << j.frame.list_generation
        << ",\n  \"sequence_id\": " << captureQuote(captured.sequenceId) << ",\n  \"capture_ordinal\": " << captured.ordinal << ",\n  \"requested_capture_count\": " << captured.requested << ",\n  \"submitted_tick_ms\": " << captured.submittedTick << ",\n  \"submitted_filetime_100ns\": " << captured.submittedFileTime
        << ",\n  \"seed\": " << j.params.seed << ",\n  \"reset\": " << (j.params.historyValid?"false":"true") << ",\n  \"history_frame_ids\": [" << (j.params.historyValid?std::to_string(captured.historyFrame):"") << "],\n  \"history_submission_id\": " << captured.historySubmission
        << ",\n  \"pre_exposure\": " << j.pack.preExposure << ",\n  \"exposure_scale\": " << j.pack.exposureScale << ",\n  \"gpu_exposure\": " << gpuExposure << ",\n  \"exposure_available\": " << (j.pack.exposureAvailable?"true":"false")
        << ",\n  \"jitter\": [" << j.metadata.jitter_x << ',' << j.metadata.jitter_y << "],\n  \"jitter_delta_pixels\": [" << j.pack.jitterDeltaX << ',' << j.pack.jitterDeltaY << "],\n  \"jitter_convention\": \"current render-pixel jitter; packed motion adds previous-minus-current delta\""
        << ",\n  \"motion_scale\": [" << j.pack.motionScaleX << ',' << j.pack.motionScaleY << "],\n  \"motion_extent\": [" << j.pack.motionWidth << ',' << j.pack.motionHeight << "],\n  \"color_rectangle\": [" << j.pack.colorX << ',' << j.pack.colorY << ',' << width << ',' << height << "],\n  \"motion_rectangle\": [" << j.pack.motionX << ',' << j.pack.motionY << ',' << j.pack.motionWidth << ',' << j.pack.motionHeight << ']'
        << ",\n  \"controls\": {\"automatic_mask\":" << j.params.autoMask << ",\"style\":" << j.params.style << ",\"tone\":" << j.params.tone << ",\"structure\":" << j.params.structure << ",\"skin\":" << j.params.skin << ",\"paper_white\":" << j.params.paperWhite << ",\"intensity\":" << j.params.intensity << ",\"blend_scale\":" << j.params.blendScale << ",\"history_strength\":" << j.params.historyStrength << ",\"enabled\":" << j.params.enabled << ",\"color_strength\":" << j.params.colorStrength << ",\"max_ratio\":" << j.params.maxRatio << '}'
        << ",\n  \"modelManifestSha256\": \"" << modelHash << "\",\n  \"shaderSha256\": \"" << kernels->shaderSha256() << "\",\n  \"baselineShaderSha256\": \"" << kernels->baselineShaderSha256() << "\",\n  \"preprocessSpvSha256\": \"" << preprocessHash << "\",\n  \"compositeSpvSha256\": \"" << compositeHash << "\",\n  \"device\": " << captureQuote(context->deviceName()) << ",\n  \"driver\": " << captureQuote(capabilities.driverName+" "+capabilities.driverInfo)
        << ",\n  \"domain\": \"scene-linear original exposure domain\",\n  \"filesSha256\": {";
      for(uint32_t i=0;i<5;i++)manifest << (i?",":"") << "\n    \"" << names[i] << "\": \"" << fileHash(staged/names[i]) << "\"";
      manifest << ",\n    \"controls.bin\": \"" << fileHash(staged/"controls.bin") << "\",\n    \"pack-controls.bin\": \"" << fileHash(staged/"pack-controls.bin") << "\"\n  }\n}\n";manifest.close();if(!manifest)throw std::runtime_error("capture manifest write failed");
      std::filesystem::rename(staged,destination);
      std::ofstream(root/"runtime.log",std::ios::app) << "bounded diagnostic capture " << captured.ordinal+1 << '/' << captured.requested << ": " << destination.string() << "; excluded from performance results\n";
    }catch(const std::exception& e){std::ofstream(root/"runtime.log",std::ios::app) << "diagnostic capture failed: " << e.what() << '\n';}
    j.capture.reset();
  }
  void prepare(){
    if(prepared)return;if(context)throw std::runtime_error("previous session preparation failed; recreate the session");LUID luid=d3d->GetAdapterLuid();context=std::make_unique<vk::Context>(vk::Backend::AmdFast,reinterpret_cast<uint8_t*>(&luid),true);context->ensureDescriptorPoolCount(kRuntimeFrameSlots);
    produced=std::make_unique<SharedFence>();finished=std::make_unique<SharedFence>();produced->create(d3d.Get(),*context);finished->create(d3d.Get(),*context);
    check(d3d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&consumed)),"consumer fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("CreateEvent failed");
    packer.create(d3d.Get(),root/"shaders"/"bridge.hlsl");model=std::make_unique<nr::Model>(*context,(root/"model").string(),true);
    if(model->blockCount()!=nr::Graph::kBlockCount)throw std::runtime_error("unsupported model block count");
    kernels=std::make_unique<nr::Kernels>(*context,(root/"shaders").string());blendScale=num::f16ToF32(nr::auxHalf(model->tensor(70,0,"blend_scale"),0,0));
    const auto preSource=shader_identity::read((root/"shaders/game_preprocess.spv").string());
    const auto compositeSource=shader_identity::read((root/"shaders/game_composite.spv").string());
    preprocess=capturedShaderPipeline(*context,preSource,"game preprocess");
    composite=capturedShaderPipeline(*context,compositeSource,"game composite");
    modelHash=captureIdentityHash(model->manifestSha256());preprocessHash=captureShaderHash(preSource);compositeHash=captureShaderHash(compositeSource);
    prepared=true;std::ofstream log(root/"runtime.log",std::ios::app);log << context->capabilityReport() << "arithmetic: " << context->arithmeticMode() << "\nmodel manifest SHA-256: " << modelHash << "\n";
    // Opt-in diagnostic metadata only. Images and model weights never leave the GPU here.
    // The DXGI sample covers this process's local segment, sampled every 60 completed jobs.
    if(std::filesystem::exists(root/"record-timings.flag")){
      ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
      if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter))))adapter.As(&memoryAdapter);
      const auto path=root/"gpu-timings.csv";const bool header=!std::filesystem::exists(path)||std::filesystem::file_size(path)==0;
      timingTrace.open(path,std::ios::app);timingTrace << std::setprecision(12);
      if(header)timingTrace << "frame_id,pack_ms,preprocess_ms,inference_ms,composite_ms,unpack_ms,neural_ms,vram_mib,allocated_neural_bytes,submitted_frames,bypassed_frames\n";
      log << "completed GPU metadata trace: " << path.string() << "; VRAM is DXGI process local usage sampled every 60 completed NR jobs\n";
    }
  }
  void resize(uint32_t w,uint32_t h){
    if(width==w&&height==h)return;drain();
    // Replacement releases the old extent before allocating the new one. Any
    // allocation failure leaves the session unusable until it is recreated.
    failed=true;jobs={};graph.reset();features=nullptr;for(auto& b:histories)context->destroyBuffer(b);for(uint32_t slot=0;slot<kRuntimeFrameSlots;++slot)context->resetDescriptorPool(slot);
    geometry=nr::Geometry::fromValid(w,h);nr::Graph::Options options;options.fusedBlocks=false;
    graph=std::make_unique<nr::Graph>(*context,*model,*kernels,geometry,options);features=graph->allocate("game features",geometry.fullWidth*geometry.fullHeight,16,nr::Format::F32);
    const auto& amdPolicy=kernels->amdPolicy();std::ofstream(root/"runtime.log",std::ios::app)
      << "session geometry " << w << 'x' << h << " padded " << geometry.fullWidth << 'x' << geometry.fullHeight
      << "; AMD kernels " << kernels->selectedKernelMode() << "; arithmetic " << amdPolicy.arithmeticName()
      << "; GEMM " << amdPolicy.gemmName()
      << "; tile N" << amdPolicy.tileN << "/K" << amdPolicy.stageK << "; fusion " << amdPolicy.fusion
      << "; FFN32 fusion " << amdPolicy.ffn32Enabled() << "; QKV32 fusion " << amdPolicy.qkv32Enabled()
      << "; window queries " << amdPolicy.windowQueries
      << "; window layout " << amdPolicy.windowLayoutName()
      << "; expert fusion " << amdPolicy.expertFusion << "; block fusion " << amdPolicy.blockFusion
      << "; experimental hardware publication " << amdPolicy.hardwarePublication
      << "; shader SHA-256 " << kernels->shaderSha256() << "; preserving baseline shaders " << kernels->baselineShaderSha256() << '\n';
    for(auto& b:histories){b=context->createBuffer(uint64_t(w)*h*16,false,"neural history");context->fillZero(b);}width=w;height=h;historyValid=false;reset=true;parity=0;lastPreparedValue=lastPreparedFrame=0;lastPreparedJitterX=lastPreparedJitterY=0;failed=false;
  }
  void reclaim(Job& j){
    if(j.retired&&consumed->GetCompletedValue()>=j.consumerValue){
      if(j.vulkanSubmitted&&j.timestamps){uint64_t ts[4]{};if(vkGetQueryPoolResults(context->device(),j.timestamps,0,4,sizeof(ts),ts,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
        OpenNrTimings t{};t.struct_size=sizeof(t);t.abi_version=1;t.frame_id=j.frame.frame_id;t.submitted_frames=submittedFrames;t.bypassed_frames=bypassedFrames;t.allocated_neural_bytes=graph->activationBytes();for(auto& h:histories)t.allocated_neural_bytes+=h.size;
        double period=context->timestampPeriodNs()/1e6;t.preprocess_ms=(ts[1]-ts[0])*period;t.inference_ms=(ts[2]-ts[1])*period;t.composite_ms=(ts[3]-ts[2])*period;
        UINT64 frequency=0;uint64_t* d3dTs=nullptr;D3D12_RANGE read{0,32};if(j.actualQueue&&SUCCEEDED(j.actualQueue->GetTimestampFrequency(&frequency))&&frequency&&SUCCEEDED(j.bridgeReadback->Map(0,&read,reinterpret_cast<void**>(&d3dTs)))){t.pack_ms=double(d3dTs[1]-d3dTs[0])*1000/frequency;t.unpack_ms=double(d3dTs[3]-d3dTs[2])*1000/frequency;t.nr_bridge_ms=double(d3dTs[3]-d3dTs[0])*1000/frequency;D3D12_RANGE noWrite{};j.bridgeReadback->Unmap(0,&noWrite);}
        if(!t.nr_bridge_ms)t.nr_bridge_ms=t.pack_ms+t.preprocess_ms+t.inference_ms+t.composite_ms+t.unpack_ms;
        // Diagnostic image copies alter GPU spans and publication can stall the
        // CPU. Captured jobs never enter ordinary timing traces or summaries.
        if(!j.capture){lastTimings=t;traceCompleted(t);timings.push_back(t.nr_bridge_ms);if(timings.size()>120)timings.erase(timings.begin());}
      }}
      if(j.vulkanSubmitted)publishCapture(j);else j.capture.reset();
      j.color.Reset();j.motion.Reset();j.exposure.Reset();j.recoveryList.Reset();j.recoveryAllocator.Reset();j.state=LMXXF_NR_JOB_NONE;j.retired=false;
    }
  }
  void initializeJob(Job& j){
    if(j.initialized)return;if(j.input){failed=true;throw std::runtime_error("previous frame buffer allocation failed; recreate the session");}j.device=context->device();uint64_t n=uint64_t(width)*height;
    j.input=std::make_unique<SharedBuffer>();j.output=std::make_unique<SharedBuffer>();j.input->create(d3d.Get(),*context,(n*2+1)*16);j.output->create(d3d.Get(),*context,(n+1)*16);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=width;rd.Height=height;rd.DepthOrArraySize=rd.MipLevels=1;rd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(d3d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&j.texture)),"private FSR input");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    for(auto& h:j.heaps)check(d3d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&h)),"bridge descriptors");
    D3D12_QUERY_HEAP_DESC queries{};queries.Count=4;queries.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;check(d3d->CreateQueryHeap(&queries,IID_PPV_ARGS(&j.bridgeQueries)),"bridge timestamp queries");
    heap.Type=D3D12_HEAP_TYPE_READBACK;rd={};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=32;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;check(d3d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&j.bridgeReadback)),"timestamp readback");
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pi.queueFamilyIndex=context->queueFamily();VK_CHECK(vkCreateCommandPool(context->device(),&pi,nullptr,&j.pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=j.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;VK_CHECK(vkAllocateCommandBuffers(context->device(),&ai,&j.commands));j.timestamps=context->createTimestampPool(4);
    j.initialized=true;
  }
};
MochizukiNrControls defaults(){MochizukiNrControls c{};c.struct_size=sizeof(c);c.intensity=c.local_tone=c.local_structure=c.history_strength=c.white_point=1;c.skin_structure=-1;c.automatic_mask=c.apply_model=1;c.max_ratio=4;return c;}
void descriptors(Session& s,Job& j){
  UINT step=s.d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu=[&](uint32_t heap,uint32_t index){auto h=j.heaps[heap]->GetCPUDescriptorHandleForHeapStart();h.ptr+=index*step;return h;};
  auto srvTexture=[&](ID3D12Resource* r,uint32_t index){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=r->GetDesc().Format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;s.d3d->CreateShaderResourceView(r,&d,cpu(0,index));};
  srvTexture(j.color.Get(),0);srvTexture(j.motion?j.motion.Get():j.color.Get(),1);srvTexture(j.exposure?j.exposure.Get():j.color.Get(),2);
  D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
  u.Buffer.NumElements=UINT(uint64_t(s.width)*s.height*2+1);u.Buffer.StructureByteStride=16;s.d3d->CreateUnorderedAccessView(j.input->d3d.Get(),nullptr,&u,cpu(0,3));
  D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Buffer.NumElements=s.width*s.height+1;v.Buffer.StructureByteStride=16;
  s.d3d->CreateShaderResourceView(j.output->d3d.Get(),&v,cpu(1,0));
  v={};v.Format=j.color->GetDesc().Format;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
  s.d3d->CreateShaderResourceView(j.color.Get(),&v,cpu(1,1));s.d3d->CreateShaderResourceView(j.color.Get(),&v,cpu(1,2));
  u={};u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;s.d3d->CreateUnorderedAccessView(j.texture.Get(),nullptr,&u,cpu(1,3));
  u={};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;u.Buffer.FirstElement=uint64_t(s.width)*s.height*4;u.Buffer.NumElements=4;u.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;s.d3d->CreateUnorderedAccessView(j.output->d3d.Get(),nullptr,&u,cpu(0,4));
}
Job& ownedJob(Session& s,void* pointer){for(auto& j:s.jobs)if(j.get()==pointer)return *j;throw std::runtime_error("job does not belong to this session");}
bool colorFormat(DXGI_FORMAT f){return f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R32G32B32A32_FLOAT||f==DXGI_FORMAT_R11G11B10_FLOAT;}
bool motionFormat(DXGI_FORMAT f){return f==DXGI_FORMAT_R16G16_FLOAT||f==DXGI_FORMAT_R32G32_FLOAT||f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R32G32B32A32_FLOAT;}
int32_t capabilities(LmxxfNrCapabilities* c){if(!c||c->struct_size<sizeof(*c))return LMXXF_NR_INVALID_ARGUMENT;*c={sizeof(*c),1,3840,2160,1,0,0,1,1};return 0;}
int32_t create(const LmxxfNrCreateInfo* i,void** out){if(!i||!out||i->struct_size<sizeof(*i)||!i->device||!i->queue||(i->flags&~LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK))return LMXXF_NR_INVALID_ARGUMENT;*out=nullptr;return guarded([&]{auto s=std::make_unique<Session>();s->d3d=static_cast<ID3D12Device*>(i->device);s->queue=static_cast<ID3D12CommandQueue*>(i->queue);if(s->queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||!deviceMatches(s->queue.Get(),s->d3d.Get()))throw std::runtime_error("[unsupported] a direct queue on the supplied D3D12 device is required");s->root=std::filesystem::path(i->assets_directory?i->assets_directory:L".")/"open-nr";wchar_t env[32768];if(GetEnvironmentVariableW(L"OPEN_NR_ASSETS",env,32768))s->root=env;s->controls=defaults();s->nextMetadata={sizeof(OpenNrFrameMetadata),1};*out=s.release();});}
int32_t prepareSession(void* p){if(!p)return LMXXF_NR_INVALID_ARGUMENT;return guarded([&]{auto& s=*(Session*)p;std::lock_guard l(s.mutex);s.prepare();});}
int32_t prepareFrame(void* p,const LmxxfNrFrameInfo* f,LmxxfNrJob* out){
  if(!p||!f||!out||f->struct_size<sizeof(*f)||out->struct_size<sizeof(*out)||!f->color||!f->color_width||!f->color_height)return LMXXF_NR_INVALID_ARGUMENT;out->handle=out->private_output=nullptr;
  auto& s=*(Session*)p;std::lock_guard lock(s.mutex);
  auto unavailable=[&](const char* e){s.invalidateTemporal();s.bypassedFrames++;errorText=e;return LMXXF_NR_UNAVAILABLE;};
  if(s.failed)return unavailable("runtime allocation or inference failed; recreate the session");
  if(!s.controlsValid)return unavailable("[unsupported] controls require an unimplemented mode");
  if(f->flags&~(LMXXF_NR_FRAME_FLAG_STRENGTH|LMXXF_NR_FRAME_FLAG_TEMPORAL))return unavailable("[unsupported] frame flags request an unavailable mode");
  if(f->debug_view||(f->flags&LMXXF_NR_FRAME_FLAG_SMOOTH_RESIDUAL)||f->smooth_strength!=0||((f->flags&LMXXF_NR_FRAME_FLAG_STRENGTH)&&f->transfer_strength!=1))return unavailable("[unsupported] use model intensity for effect strength; compatibility debug/smoothing/detail modes are unavailable");
  if(f->color_width>3840||f->color_height>2160)return unavailable("[unsupported] input extent exceeds the first-release limit");
  if(f->passes>1||(f->model_scale!=0&&f->model_scale!=1))return unavailable("one full render-resolution NR pass is required");
  auto color=(ID3D12Resource*)f->color;auto rd=color->GetDesc();if(!colorFormat(rd.Format)||rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||rd.DepthOrArraySize!=1||rd.SampleDesc.Count!=1||rd.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)return unavailable("[unsupported] color format or resource flags");
  if(!deviceMatches(color,s.d3d.Get()))return unavailable("[unsupported] color belongs to a different D3D12 device");
  if(uint64_t(s.nextMetadata.color_x)+f->color_width>rd.Width||uint64_t(s.nextMetadata.color_y)+f->color_height>rd.Height)return unavailable("invalid color rectangle");
  for(auto& j:s.jobs)if(j){s.reclaim(*j);if((s.width!=f->color_width||s.height!=f->color_height)&&j->state!=LMXXF_NR_JOB_NONE)return unavailable("waiting for previous extent's consumers");}
  if(std::all_of(s.jobs.begin(),s.jobs.end(),[](const auto& j){return j&&j->state!=LMXXF_NR_JOB_NONE;}))return unavailable("all frame slots are busy; retain ordinary FSR");
  Job* pending=nullptr;
  const int32_t result=guarded([&]{
    s.prepare();s.resize(f->color_width,f->color_height);Job* j=nullptr;uint32_t slot=0;
    for(;slot<s.jobs.size();slot++){if(!s.jobs[slot])s.jobs[slot]=std::make_unique<Job>();if(s.jobs[slot]->state==LMXXF_NR_JOB_NONE){j=s.jobs[slot].get();break;}}
    if(!j)throw std::runtime_error("all frame slots are busy");pending=j;s.initializeJob(*j);j->color.Reset();j->motion.Reset();j->exposure.Reset();j->frame=*f;j->metadata=s.nextMetadata;j->controls=s.controls;j->color=color;
    if(f->motion&&(f->flags&LMXXF_NR_FRAME_FLAG_TEMPORAL)){auto m=(ID3D12Resource*)f->motion;auto d=m->GetDesc();if(!deviceMatches(m,s.d3d.Get())||!motionFormat(d.Format)||d.SampleDesc.Count!=1||d.DepthOrArraySize!=1||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))throw std::runtime_error("[unsupported] motion resource");j->motion=m;}
    if(j->metadata.exposure){auto e=(ID3D12Resource*)j->metadata.exposure;auto d=e->GetDesc();if(deviceMatches(e,s.d3d.Get())&&d.SampleDesc.Count==1&&d.DepthOrArraySize==1&&d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&!(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)&&(d.Format==DXGI_FORMAT_R16_FLOAT||d.Format==DXGI_FORMAT_R32_FLOAT))j->exposure=e;else throw std::runtime_error("[unsupported] exposure texture");}
    uint32_t mw=f->motion_width?f->motion_width:s.width,mh=f->motion_height?f->motion_height:s.height;
    if(j->motion&&(uint64_t(mw)+j->metadata.motion_x>j->motion->GetDesc().Width||uint64_t(mh)+j->metadata.motion_y>j->motion->GetDesc().Height))throw std::runtime_error("invalid motion rectangle");
    j->value=++s.counter;j->consumerValue=0;j->retired=false;j->vulkanSubmitted=false;j->outputsRecorded=false;j->recovered=false;j->actualQueue.Reset();j->state=LMXXF_NR_JOB_PREPARED;
    // Pack constants are immutable once the D3D producer is recorded. Pair
    // jitter with the immediately preceding successful preparation; Enqueue
    // validates that exact predecessor against actual submission order.
    j->packedPredecessor=s.lastPreparedValue;j->packedConsecutive=s.lastPreparedValue&&f->frame_id==s.lastPreparedFrame+1;
    j->pack={s.width,s.height,j->metadata.color_x,j->metadata.color_y,mw,mh,j->metadata.motion_x,j->metadata.motion_y,f->motion_scale_x,f->motion_scale_y,sane(j->metadata.pre_exposure>0?j->metadata.pre_exposure:1,1e-8f,1e8f,1),sane(j->metadata.exposure_scale>0?j->metadata.exposure_scale:1,1e-8f,1e8f,1),j->packedConsecutive?s.lastPreparedJitterX-j->metadata.jitter_x:0,j->packedConsecutive?s.lastPreparedJitterY-j->metadata.jitter_y:0,uint32_t(bool(j->motion)),uint32_t(bool(j->exposure))};
    j->params={s.geometry.fullWidth,s.geometry.fullHeight,s.width,s.height,0,0,j->controls.automatic_mask,std::min(j->controls.style,2u),sane(j->controls.local_tone,0,2,1),sane(j->controls.local_structure,0,2,1),sane(j->controls.skin_structure,-1,2,-1),sane(j->controls.white_point,.05,100,1),sane(j->controls.intensity,0,2,1),s.blendScale,sane(j->controls.history_strength,0,1,1),float(j->controls.apply_model!=0),sane(f->color_strength,0,4,1),sane(j->controls.max_ratio,1,16,4)};
    descriptors(s,*j);s.lastPreparedValue=j->value;s.lastPreparedFrame=f->frame_id;s.lastPreparedJitterX=j->metadata.jitter_x;s.lastPreparedJitterY=j->metadata.jitter_y;out->handle=j;out->private_output=j->texture.Get();
  });
  if(result!=LMXXF_NR_OK){s.invalidateTemporal();s.bypassedFrames++;if(pending){pending->state=LMXXF_NR_JOB_NONE;pending->color.Reset();pending->motion.Reset();pending->exposure.Reset();}out->handle=out->private_output=nullptr;}
  return result;
}
int32_t recordInputs(void* p,void* job,void* list){if(!p||!job||!list)return LMXXF_NR_INVALID_ARGUMENT;return guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto& j=ownedJob(s,job);if(j.state!=LMXXF_NR_JOB_PREPARED)throw std::runtime_error("RecordInputs out of order");auto cmd=(ID3D12GraphicsCommandList*)list;
  cmd->EndQuery(j.bridgeQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
  transition(cmd,j.color.Get(),D3D12_RESOURCE_STATES(j.frame.color_state),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if(j.motion)transition(cmd,j.motion.Get(),D3D12_RESOURCE_STATES(j.frame.motion_state),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if(j.exposure)transition(cmd,j.exposure.Get(),D3D12_RESOURCE_STATES(j.metadata.exposure_state),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(cmd,j.input->d3d.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heap=j.heaps[0].Get();cmd->SetDescriptorHeaps(1,&heap);auto gpu=heap->GetGPUDescriptorHandleForHeapStart();auto cpu=heap->GetCPUDescriptorHandleForHeapStart();UINT step=s.d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);gpu.ptr+=4*step;cpu.ptr+=4*step;
  transition(cmd,j.output->d3d.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);const UINT zero[4]{};cmd->ClearUnorderedAccessViewUint(gpu,cpu,j.output->d3d.Get(),zero,0,nullptr);transition(cmd,j.output->d3d.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
  cmd->SetComputeRootSignature(s.packer.root.Get());cmd->SetPipelineState(s.packer.pack.Get());cmd->SetComputeRoot32BitConstants(0,16,&j.pack,0);cmd->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());cmd->Dispatch((s.width+7)/8,(s.height+7)/8,1);
  transition(cmd,j.input->d3d.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
  transition(cmd,j.color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATES(j.frame.color_state));if(j.motion)transition(cmd,j.motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATES(j.frame.motion_state));if(j.exposure)transition(cmd,j.exposure.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATES(j.metadata.exposure_state));j.state=LMXXF_NR_JOB_PRODUCER_SUBMITTED;
  cmd->EndQuery(j.bridgeQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);cmd->ResolveQueryData(j.bridgeQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,j.bridgeReadback.Get(),0);
});}
void dispatchFrame(Session& s,Job& j,vk::Pipeline pipeline,bool composite){
  const vk::Buffer* bindings[vk::kGenericBindings]{};bindings[0]=&j.input->buffer;bindings[1]=&s.histories[s.parity];if(composite)bindings[2]=&s.graph->head().buffer;bindings[3]=&j.output->buffer;bindings[4]=&s.histories[s.parity^1];bindings[7]=&s.features->buffer;
  auto set=s.context->allocateSet(bindings);vkCmdBindPipeline(j.commands,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);vkCmdBindDescriptorSets(j.commands,VK_PIPELINE_BIND_POINT_COMPUTE,s.context->pipelineLayout(),0,1,&set,0,nullptr);vkCmdPushConstants(j.commands,s.context->pipelineLayout(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(j.params),&j.params);vkCmdDispatch(j.commands,((composite?s.width:s.geometry.fullWidth)+7)/8,((composite?s.height:s.geometry.fullHeight)+7)/8,1);
}
int32_t enqueue(void* p,void* job,void* queue){if(!p||!job||!queue)return LMXXF_NR_INVALID_ARGUMENT;bool queueValidated=false;int32_t rc=guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto& j=ownedJob(s,job);if(j.state!=LMXXF_NR_JOB_PRODUCER_SUBMITTED&&j.state!=LMXXF_NR_JOB_CONSUMER_COMPLETE)throw std::runtime_error("Enqueue requires recorded inputs");auto q=(ID3D12CommandQueue*)queue;if(q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||!deviceMatches(q,s.d3d.Get())||!sameQueue(q,s.queue.Get()))throw std::runtime_error("submission queue must be the session's direct D3D12 queue");if(j.value<=s.lastSubmittedValue)throw std::runtime_error("prepared jobs must be submitted in increasing preparation order");queueValidated=true;
  const bool valid=s.historyValid&&!s.reset&&!j.frame.reset&&j.motion&&j.packedConsecutive&&j.packedPredecessor==s.lastSubmittedValue&&j.frame.frame_id==s.lastFrame+1;
  j.params.historyValid=uint32_t(valid);j.params.seed=valid?uint32_t(s.frames):0;
  check(q->Signal(s.produced->fence.Get(),j.value),"input ready fence");uint32_t slot=0;while(slot<s.jobs.size()&&s.jobs[slot].get()!=&j)++slot;s.context->resetDescriptorPool(slot);VK_CHECK(vkResetCommandPool(s.context->device(),j.pool,0));VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK_CHECK(vkBeginCommandBuffer(j.commands,&b));
  vkCmdResetQueryPool(j.commands,j.timestamps,0,4);vkCmdWriteTimestamp(j.commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,j.timestamps,0);
  VkBufferMemoryBarrier2 external[2]{};for(uint32_t i=0;i<2;i++){auto& x=external[i];x.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;x.srcStageMask=VK_PIPELINE_STAGE_2_NONE;x.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;x.dstAccessMask=i?VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT:VK_ACCESS_2_SHADER_STORAGE_READ_BIT;x.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;x.dstQueueFamilyIndex=s.context->queueFamily();x.buffer=i?j.output->buffer.buffer:j.input->buffer.buffer;x.size=VK_WHOLE_SIZE;}
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.bufferMemoryBarrierCount=2;dep.pBufferMemoryBarriers=external;vkCmdPipelineBarrier2(j.commands,&dep);
  const auto capturePath=s.root/"capture.flag";const bool captureFlag=std::filesystem::exists(capturePath);if(!captureFlag)s.captureRequest.removed();
  if(captureFlag&&!s.captureRequest.latched){
    try{std::ifstream flag(capturePath,std::ios::binary);if(!flag)throw std::runtime_error("capture.flag cannot be read");char contents[65]{};flag.read(contents,sizeof(contents));const auto length=flag.gcount();if(length>64)throw std::runtime_error("capture.flag exceeds 64 bytes");s.captureRequest.activate(std::string_view(contents,size_t(length)));s.captureSequenceId="sequence-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(++s.captureSequenceCounter);}
    catch(const std::exception& e){s.captureRequest.latched=true;s.captureRequest.requested=s.captureRequest.remaining=0;std::ofstream(s.root/"runtime.log",std::ios::app) << "bounded capture request rejected: " << e.what() << '\n';}
  }
  if(s.captureRequest.remaining){
    const auto ordinal=s.captureRequest.reserve();
    try{j.capture=std::make_unique<FrameCapture>();j.capture->sequenceId=s.captureSequenceId;j.capture->ordinal=ordinal;j.capture->requested=s.captureRequest.requested;j.capture->historyFrame=valid?s.lastFrame:0;j.capture->historySubmission=valid?s.lastSubmittedValue:0;j.capture->submittedTick=GetTickCount64();FILETIME filetime;GetSystemTimePreciseAsFileTime(&filetime);j.capture->submittedFileTime=uint64_t(filetime.dwLowDateTime)|(uint64_t(filetime.dwHighDateTime)<<32);const uint64_t n=uint64_t(s.width)*s.height;j.capture->allocate(*s.context,0,(n*2+1)*16);j.capture->allocate(*s.context,1,s.features->buffer.size);j.capture->allocate(*s.context,2,s.histories[s.parity].size);j.capture->allocate(*s.context,4,n*16);}
    catch(const std::exception& e){j.capture.reset();std::ofstream(s.root/"runtime.log",std::ios::app) << "diagnostic capture allocation failed: " << e.what() << '\n';}
  }
  s.context->computeBarrier(j.commands);dispatchFrame(s,j,s.preprocess,false);s.context->computeBarrier(j.commands);vkCmdWriteTimestamp(j.commands,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,j.timestamps,1);
  if(j.capture){
    VkMemoryBarrier2 toCopy{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};toCopy.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;toCopy.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;toCopy.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;toCopy.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;VkDependencyInfo copyDep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};copyDep.memoryBarrierCount=1;copyDep.pMemoryBarriers=&toCopy;vkCmdPipelineBarrier2(j.commands,&copyDep);
    j.capture->copy(j.commands,0,j.input->buffer);j.capture->copy(j.commands,1,s.features->buffer);j.capture->copy(j.commands,2,s.histories[s.parity]);
    toCopy.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;toCopy.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;toCopy.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;toCopy.dstAccessMask=VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;vkCmdPipelineBarrier2(j.commands,&copyDep);
  }
  s.graph->record(j.commands,*s.features);s.context->computeBarrier(j.commands);vkCmdWriteTimestamp(j.commands,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,j.timestamps,2);dispatchFrame(s,j,s.composite,true);
  VkMemoryBarrier2 ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};ready.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;ready.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;ready.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;ready.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;VkDependencyInfo readyDep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};readyDep.memoryBarrierCount=1;readyDep.pMemoryBarriers=&ready;vkCmdPipelineBarrier2(j.commands,&readyDep);
  if(j.capture){try{j.capture->allocate(*s.context,3,s.graph->head().buffer.size);j.capture->copy(j.commands,3,s.graph->head().buffer);j.capture->copy(j.commands,4,j.output->buffer);}catch(const std::exception& e){j.capture->complete=false;std::ofstream(s.root/"runtime.log",std::ios::app) << "diagnostic capture head allocation failed: " << e.what() << '\n';}}
  const uint32_t marker[4]={0x3f800000u,0,0,0};vkCmdUpdateBuffer(j.commands,j.output->buffer.buffer,uint64_t(s.width)*s.height*16,16,marker);
  if(j.capture){ready.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;ready.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;ready.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;ready.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;vkCmdPipelineBarrier2(j.commands,&readyDep);}
  for(auto& x:external){x.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_TRANSFER_BIT;x.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT;x.dstStageMask=VK_PIPELINE_STAGE_2_NONE;x.dstAccessMask=0;std::swap(x.srcQueueFamilyIndex,x.dstQueueFamilyIndex);}vkCmdPipelineBarrier2(j.commands,&dep);vkCmdWriteTimestamp(j.commands,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,j.timestamps,3);VK_CHECK(vkEndCommandBuffer(j.commands));
  VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timeline.waitSemaphoreValueCount=timeline.signalSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=timeline.pSignalSemaphoreValues=&j.value;VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,&timeline};submit.waitSemaphoreCount=submit.signalSemaphoreCount=1;submit.pWaitSemaphores=&s.produced->semaphore;submit.pWaitDstStageMask=&stage;submit.pSignalSemaphores=&s.finished->semaphore;submit.commandBufferCount=1;submit.pCommandBuffers=&j.commands;VK_CHECK(vkQueueSubmit(s.context->queue(),1,&submit,VK_NULL_HANDLE));j.vulkanSubmitted=true;s.lastSubmittedValue=j.value;check(q->Wait(s.finished->fence.Get(),j.value),"neural ready GPU wait");
  j.actualQueue=q;j.state=LMXXF_NR_JOB_NR_ENQUEUED;s.parity^=1;s.historyValid=true;s.reset=false;s.lastFrame=j.frame.frame_id;s.lastJitterX=j.metadata.jitter_x;s.lastJitterY=j.metadata.jitter_y;if(!valid)s.frames=0;s.frames++;s.submittedFrames++;
});
  if(rc!=LMXXF_NR_OK){auto& s=*(Session*)p;std::lock_guard lock(s.mutex);for(auto& pointer:s.jobs)if(pointer.get()==job){auto& j=*pointer;
    if(j.vulkanSubmitted){s.failed=true;s.reset=true;}
    if(queueValidated&&!j.vulkanSubmitted&&j.state==LMXXF_NR_JOB_CONSUMER_COMPLETE){
      // Producer cleared the output marker. The consumer reads original scene color on failure.
      s.failed=true;s.reset=true;j.actualQueue=(ID3D12CommandQueue*)queue;j.state=LMXXF_NR_JOB_NR_ENQUEUED;return LMXXF_NR_OK;
    }
  }}
  return rc;
}
int32_t recordOutputs(void* p,void* job,void* list){if(!p||!job||!list)return LMXXF_NR_INVALID_ARGUMENT;return guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto& j=ownedJob(s,job);if(j.state!=LMXXF_NR_JOB_PRODUCER_SUBMITTED&&j.state!=LMXXF_NR_JOB_NR_ENQUEUED)throw std::runtime_error("RecordOutputs out of order");if(j.outputsRecorded)throw std::runtime_error("RecordOutputs called twice");auto cmd=(ID3D12GraphicsCommandList*)list;transition(cmd,j.output->d3d.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(cmd,j.texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd->EndQuery(j.bridgeQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);
  transition(cmd,j.color.Get(),D3D12_RESOURCE_STATES(j.frame.color_state),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap* heap=j.heaps[1].Get();cmd->SetDescriptorHeaps(1,&heap);cmd->SetComputeRootSignature(s.packer.root.Get());cmd->SetPipelineState(s.packer.unpack.Get());cmd->SetComputeRoot32BitConstants(0,16,&j.pack,0);cmd->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());cmd->Dispatch((s.width+7)/8,(s.height+7)/8,1);transition(cmd,j.texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(cmd,j.output->d3d.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);transition(cmd,j.color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATES(j.frame.color_state));j.outputsRecorded=true;j.state=j.actualQueue?LMXXF_NR_JOB_NR_ENQUEUED:LMXXF_NR_JOB_CONSUMER_COMPLETE;
  cmd->EndQuery(j.bridgeQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,3);cmd->ResolveQueryData(j.bridgeQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2,2,j.bridgeReadback.Get(),16);
});}
int32_t recoverSubmission(void* p,void* job,void* queue){
  if(!p||!job||!queue)return LMXXF_NR_INVALID_ARGUMENT;
  return guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto& j=ownedJob(s,job);auto q=(ID3D12CommandQueue*)queue;
    if(q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||!deviceMatches(q,s.d3d.Get())||!j.initialized||j.state==LMXXF_NR_JOB_NONE||j.retired||j.recoveryAllocator)throw std::runtime_error("invalid fallback recovery boundary");
    s.failed=true;s.reset=true;
    // Only the host's isolated pending job may use this failure path. Finish
    // older consumers first so the shared lifetime fence cannot complete out
    // of order if the actual producer queue differs from session creation.
    for(auto& other:s.jobs)if(other&&other.get()!=&j&&other->state!=LMXXF_NR_JOB_NONE&&!other->retired)throw std::runtime_error("fallback requires all other jobs retired");
    s.waitConsumer(s.consumerCounter);s.context->waitIdle();
    for(auto& other:s.jobs)if(other&&other.get()!=&j)s.reclaim(*other);
    j.capture.reset(); // GPU work is complete; a failed frame is not a capture.
    check(s.d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&j.recoveryAllocator)),"fallback allocator");
    check(s.d3d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,j.recoveryAllocator.Get(),nullptr,IID_PPV_ARGS(&j.recoveryList)),"fallback list");
    auto cmd=j.recoveryList.Get();transition(cmd,j.output->d3d.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heap=j.heaps[0].Get();cmd->SetDescriptorHeaps(1,&heap);auto gpu=heap->GetGPUDescriptorHandleForHeapStart();auto cpu=heap->GetCPUDescriptorHandleForHeapStart();const UINT step=s.d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);gpu.ptr+=4*step;cpu.ptr+=4*step;const UINT zero[4]{};
    cmd->ClearUnorderedAccessViewUint(gpu,cpu,j.output->d3d.Get(),zero,0,nullptr);transition(cmd,j.output->d3d.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);check(cmd->Close(),"fallback close");
    ID3D12CommandList* lists[]={cmd};q->ExecuteCommandLists(1,lists);
    // The actual queue orders producer -> marker clear -> continuation. Keep
    // this allocator and every job resource until Retire's consumer fence.
    j.actualQueue=q;j.vulkanSubmitted=false;j.recovered=true;j.state=LMXXF_NR_JOB_NR_ENQUEUED;s.historyValid=false;s.frames=0;s.bypassedFrames++;
  });
}
int32_t retire(void* p,void* job){if(!p||!job)return LMXXF_NR_INVALID_ARGUMENT;return guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto& j=ownedJob(s,job);if(j.state!=LMXXF_NR_JOB_NR_ENQUEUED||!j.actualQueue||(!j.outputsRecorded&&!j.recovered))throw std::runtime_error("Retire requires submitted NR or recovered fallback and consumer");j.consumerValue=++s.consumerCounter;check(j.actualQueue->Signal(s.consumed.Get(),j.consumerValue),"consumer lifetime fence");j.retired=true;j.state=LMXXF_NR_JOB_RETIRED;});}
int32_t cancel(void* p,void* job){if(!p||!job)return LMXXF_NR_INVALID_ARGUMENT;return guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto& j=ownedJob(s,job);if(j.state==LMXXF_NR_JOB_NR_ENQUEUED||j.state==LMXXF_NR_JOB_RETIRED||j.vulkanSubmitted)throw std::runtime_error("cannot cancel submitted work; retire and drain it");j.capture.reset();j.state=LMXXF_NR_JOB_NONE;j.color.Reset();j.motion.Reset();j.exposure.Reset();s.invalidateTemporal();});}
int32_t poll(void* p,void* job,uint32_t* state){if(!p||!job||!state)return LMXXF_NR_INVALID_ARGUMENT;return guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto& j=ownedJob(s,job);*state=j.retired?LMXXF_NR_JOB_RETIRED:j.vulkanSubmitted&&s.finished->fence->GetCompletedValue()>=j.value?LMXXF_NR_JOB_NR_COMPLETE:j.state;});}
int32_t drain(void* p){if(!p)return LMXXF_NR_INVALID_ARGUMENT;return guarded([&]{auto& s=*(Session*)p;std::lock_guard lock(s.mutex);s.drain();});}
int32_t destroy(void* p){if(!p)return LMXXF_NR_INVALID_ARGUMENT;int32_t rc=drain(p);if(rc==0)delete(Session*)p;return rc;}
int32_t reset(void* p){if(!p)return LMXXF_NR_INVALID_ARGUMENT;auto& s=*(Session*)p;std::lock_guard lock(s.mutex);s.invalidateTemporal();s.frames=0;return 0;}
int32_t status(void* p,char* out,uint32_t n){if(!p||!out||!n)return LMXXF_NR_INVALID_ARGUMENT;auto& s=*(Session*)p;std::lock_guard lock(s.mutex);textOut(out,n,"OpenDLSS-NR AMD Vulkan; "+std::to_string(s.frames)+" frames; native D3D12 shared buffers; timings exclude game");return 0;}
int32_t lastError(char* out,uint32_t n){if(!out||!n)return LMXXF_NR_INVALID_ARGUMENT;textOut(out,n,errorText);return 0;}
} // namespace
extern "C" __declspec(dllexport) int32_t LmxxfNrGetApi(uint32_t version,LmxxfNrApi* out){if(!out||out->struct_size<sizeof(*out)||version!=1)return LMXXF_NR_UNSUPPORTED_ABI;*out={sizeof(*out),1,capabilities,create,destroy,prepareSession,prepareFrame,recordInputs,enqueue,recordOutputs,enqueue,cancel,poll,retire,reset,drain,status,lastError};return 0;}
extern "C" __declspec(dllexport) int32_t OpenNrSetFrameMetadata(void* p,const OpenNrFrameMetadata* m){if(!p||!m||m->struct_size<sizeof(*m)||m->abi_version!=1)return LMXXF_NR_INVALID_ARGUMENT;auto& s=*(Session*)p;std::lock_guard lock(s.mutex);s.nextMetadata=*m;return 0;}
extern "C" __declspec(dllexport) int32_t OpenNrRecoverSubmission(void* p,void* job,void* queue){return recoverSubmission(p,job,queue);}
extern "C" __declspec(dllexport) int32_t OpenNrGetTimings(void* p,OpenNrTimings* out){if(!p||!out||out->struct_size<sizeof(*out))return LMXXF_NR_INVALID_ARGUMENT;auto& s=*(Session*)p;std::lock_guard lock(s.mutex);for(auto& j:s.jobs)if(j)s.reclaim(*j);if(!s.lastTimings.struct_size)return LMXXF_NR_UNAVAILABLE;*out=s.lastTimings;out->submitted_frames=s.submittedFrames;out->bypassed_frames=s.bypassedFrames;return LMXXF_NR_OK;}
extern "C" __declspec(dllexport) int32_t OpenNrQueryDeviceCapabilities(void* p,OpenNrDeviceCapabilities* out){
  if(!p||!out||out->struct_size<sizeof(*out))return LMXXF_NR_INVALID_ARGUMENT;*out={sizeof(*out),1};
  return guarded([&]{auto d=(ID3D12Device*)p;LUID luid=d->GetAdapterLuid();vk::Context c(vk::Backend::AmdFast,reinterpret_cast<uint8_t*>(&luid),true);const auto& a=c.capabilities();
    out->vendor_id=a.properties.vendorID;out->device_id=a.properties.deviceID;out->wave_size=32;out->matrix_m=out->matrix_n=out->matrix_k=16;out->fp8_e4m3=a.fp8Matrix16;out->accumulator_fp32=1;out->max_input_width=3840;out->max_input_height=2160;out->max_storage_buffer_bytes=a.properties.limits.maxStorageBufferRange;
    VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(c.physical(),&memory);for(uint32_t i=0;i<memory.memoryHeapCount;i++)if(memory.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)out->device_local_bytes+=memory.memoryHeaps[i].size;
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&driver};vkGetPhysicalDeviceProperties2(c.physical(),&props);textOut(out->device_name,sizeof(out->device_name),a.properties.deviceName);textOut(out->driver,sizeof(out->driver),std::string(driver.driverName)+" "+driver.driverInfo);textOut(out->arithmetic_mode,sizeof(out->arithmetic_mode),"RDNA4 E4M3 K16 FP32 accumulation / FP16 publication");
    SharedBuffer testBuffer;testBuffer.create(d,c,65536);SharedFence testFence;testFence.create(d,c);out->shared_buffers=out->shared_timeline_fences=out->supported=1;
  });
}
extern "C" __declspec(dllexport) int32_t OpenNrGetApi(uint32_t version,OpenNrApi* out){if(!out||out->struct_size<sizeof(*out)||version!=1)return LMXXF_NR_UNSUPPORTED_ABI;*out={sizeof(*out),1};out->lifecycle.struct_size=sizeof(out->lifecycle);LmxxfNrGetApi(1,&out->lifecycle);out->SetFrameMetadata=OpenNrSetFrameMetadata;out->QueryDeviceCapabilities=OpenNrQueryDeviceCapabilities;out->GetTimings=OpenNrGetTimings;return 0;}
extern "C" __declspec(dllexport) uint32_t MochizukiNrGetFeatures(){return 0;} // A queue change requires host drain and recreation.
extern "C" __declspec(dllexport) int32_t MochizukiNrSetControls(void* p,const MochizukiNrControls* c){if(!p||!c||c->struct_size<offsetof(MochizukiNrControls,drs_mode))return LMXXF_NR_INVALID_ARGUMENT;auto& s=*(Session*)p;std::lock_guard lock(s.mutex);auto d=defaults();memcpy(&d,c,std::min<size_t>(c->struct_size,sizeof(d)));if(d.flags||d.preprocess||d.drs_mode||d.linear_input==2||d.pass[0].used||d.pass[1].used){s.controlsValid=false;s.reset=true;errorText="[unsupported] preprocessing, display input, dynamic buckets, and later-pass controls are unavailable";return LMXXF_NR_UNAVAILABLE;}s.controls=d;s.controlsValid=true;return 0;}
extern "C" __declspec(dllexport) int32_t MochizukiNrGetControlDefaults(MochizukiNrControls* c){if(!c||c->struct_size<sizeof(*c))return LMXXF_NR_INVALID_ARGUMENT;*c=defaults();return 0;}
extern "C" __declspec(dllexport) int32_t MochizukiNrGetInfo(void* p,MochizukiNrInfo* out){if(!p||!out||out->struct_size<sizeof(*out))return LMXXF_NR_INVALID_ARGUMENT;auto& s=*(Session*)p;std::lock_guard lock(s.mutex);MochizukiNrInfo info{};info.struct_size=sizeof(info);info.frame_w=info.model_w=s.width;info.frame_h=info.model_h=s.height;info.max_passes=1;info.frames=s.frames;info.white_point=s.controls.white_point;info.history_consumed_pct=s.historyValid?100:0;auto t=s.timings;if(!t.empty()){std::sort(t.begin(),t.end());info.gpu_ms_median=float(t[t.size()/2]);info.gpu_ms_p95=float(t[size_t((t.size()-1)*.95)]);}*out=info;return 0;}
