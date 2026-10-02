// Model-free GPU checks of the native routes against the existing CPU numeric
// oracle. These exercise real descriptor layouts, dispatches, and GPU readback.
#include "kernels.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"
#include "sha256.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
struct Buffers {
  vk::Context& context;
  std::vector<vk::Buffer> owned;
  ~Buffers() { for (auto& b : owned) context.destroyBuffer(b); }
  vk::Buffer make(size_t bytes, const void* data = nullptr) {
    auto b = context.createBuffer(bytes, false, "synthetic test");
    owned.push_back(b);
    if (data) context.upload(b, data, bytes); else context.fillZero(b);
    return b;
  }
  nr::Activation activation(uint32_t rows, uint32_t columns, nr::Format format, const void* data = nullptr) {
    nr::Activation a;
    a.rows = rows; a.allocRows = nr::alignRows(rows); a.channels = columns; a.format = format;
    a.buffer = make((size_t)a.allocRows * columns * nr::formatBytes(format), data);
    return a;
  }
};
uint32_t rng = 0x91827364u;
uint32_t randomWord() { rng = rng * 1664525u + 1013904223u; return rng; }
uint8_t randomE4() { uint32_t v = randomWord(); return uint8_t((v % 57u) | ((v >> 16) & 0x80u)); }
uint16_t randomHalf() { return num::f16Bits(float(int(randomWord() % 2049u) - 1024) / 1024.0f); }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Comparison {
  bool exact;
  uint32_t mismatches = 0;
  double squared = 0;
  float maxError = 0, maxExpected = 0;
  uint64_t count = 0;
  void sample(float actual, float expected, bool bitsMatch) {
    require(std::isfinite(actual) && std::isfinite(expected), "nonfinite synthetic output");
    if (!bitsMatch) ++mismatches;
    float error = std::fabs(actual - expected);
    maxError = std::max(maxError, error); maxExpected = std::max(maxExpected, std::fabs(expected));
    squared += double(error) * error; ++count;
  }
  void finish(const char* label) {
    printf("selftest %-24s samples=%llu different=%u maxAbs=%.6g rms=%.6g\n", label,
           (unsigned long long)count, mismatches, maxError, std::sqrt(squared / std::max<uint64_t>(count, 1)));
    if (exact) require(mismatches == 0, "reference publication differs from CPU oracle");
    else require(maxError <= std::max(0.125f, maxExpected * 0.125f), "AMD matrix result fails synthetic layout/accuracy check");
  }
};
// The WGSL encoder in ports/browser-webgpu/shaders/numerics.wgsl defines
// publication by integer fields rather than a hardware FP8 conversion. Keep a
// separate implementation here so this exhaustive check covers AMD's packed
// hardware conversions, including the signs that float-value comparisons hide.
uint8_t webgpuE4Encoder(uint16_t half) {
  if ((half & 0x7c00u) == 0x7c00u && (half & 0x03ffu)) return 0;
  const uint32_t sign = (half >> 8u) & 0x80u;
  if (!(half & 0x7fffu)) return uint8_t(sign);
  const uint32_t exponent = (half >> 10u) & 31u, mantissa = half & 1023u;
  auto rne = [](uint32_t n, uint32_t shift) {
    const uint32_t q = n >> shift, rem = n & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
    return q + uint32_t(rem > mid || (rem == mid && (q & 1u)));
  };
  uint32_t code;
  if (exponent == 31u) code = 0x7eu;
  else if (exponent <= 8u) code = std::min(rne(exponent ? 1024u + mantissa : mantissa, exponent ? 16u - exponent : 15u), 8u);
  else {
    uint32_t e = exponent - 8u, m = rne(mantissa, 7u);
    if (m == 8u) { m = 0; ++e; }
    code = e > 15u || (e == 15u && m > 6u) ? 0x7eu : (e << 3u) | m;
  }
  return uint8_t(sign | code);
}
void publicationCase(vk::Context& c, nr::Kernels& kernels) {
  Buffers storage{c}; constexpr uint32_t rows = 8192, columns = 8;
  std::vector<uint16_t> half(rows * columns);
  for (uint32_t i = 0; i < half.size(); ++i) half[i] = uint16_t(i);
  auto input = storage.activation(rows, columns, nr::Format::F16, half.data());
  auto out = storage.activation(rows, columns, nr::Format::E4);
  auto commands = c.beginCommands(); kernels.quantize(commands, input, out); c.endAndSubmit(commands, true);
  auto bytes = c.download(out.buffer, out.validBytes());
  for (uint32_t i = 0; i < half.size(); ++i) {
    const uint8_t expected = webgpuE4Encoder(half[i]);
    require(expected == num::e4m3FromF16Bits(half[i]), "CPU encoder differs from the WebGPU publication contract");
    if (bytes[i] != expected) {
      fprintf(stderr, "half publication mismatch input=0x%04x GPU=0x%02x WebGPU=0x%02x\n", i, bytes[i], expected);
      throw std::runtime_error("exhaustive half-to-E4 byte publication differs");
    }
  }
  // Explicit witnesses for zero sign, NaN canonicalization, infinity/finite
  // saturation, and a subnormal RNE tie. These expected bytes are literal.
  const uint16_t witnesses[] = {0x0000,0x8000,0x7e00,0xfe00,0x7c00,0xfc00,0x7bff,0xfbff,0x1400,0x9400,0x1800,0x9800,0x1a00,0x9a00};
  const uint8_t expected[] = {0x00,0x80,0x00,0x00,0x7e,0xfe,0x7e,0xfe,0x00,0x80,0x01,0x81,0x02,0x82};
  for (size_t i = 0; i < sizeof(witnesses) / sizeof(witnesses[0]); ++i)
    require(bytes[witnesses[i]] == expected[i], "literal E4 publication witness failed");
  printf("selftest %-24s samples=65536 different=0 (byte comparison, all half patterns)\n", "E4 zero/NaN/RNE/saturation");
}
void gameFrameCase(vk::Context& c, const std::string& shaderDirectory) {
  std::filesystem::path gameShaders(shaderDirectory);
  if (!std::filesystem::exists(gameShaders / "game_preprocess.spv"))
    gameShaders = gameShaders.parent_path() / "game" / "shaders";
  if (!std::filesystem::exists(gameShaders / "game_preprocess.spv") ||
      !std::filesystem::exists(gameShaders / "game_composite.spv")) {
    printf("selftest game frame shaders: SKIP (build scripts/build_game.ps1 to include them)\n");
    return;
  }
  Buffers storage{c}; constexpr uint32_t width = 3, height = 2, fullWidth = 8, fullHeight = 8, pixels = width * height;
  struct Pixel { float r,g,b,a; float rgb(uint32_t c) const { return c==0?r:c==1?g:b; } };
  std::vector<Pixel> source(pixels * 2 + 1), previous(pixels), head(fullWidth * fullHeight);
  for (uint32_t i = 0; i < pixels; ++i) {
    // Neutral gray isolates exposure reversal: a saturated colored proxy can
    // legitimately change hue even when the head is zero in upstream's tone
    // upgrade. Alternate SDR and bright HDR values, with nonopaque alpha.
    float gray=i%2?0.0625f*float(i+1):12.0f+float(i);
    source[i * 2] = {gray,gray,gray,0.125f * float(i + 1)};
    source[i * 2 + 1] = {0,0,1,0}; previous[i] = {.25f,.5f,.75f,1};
  }
  source[pixels * 2] = {3,0,0,0}; // Game exposure, distinct from paper white.
  auto in = storage.make(source.size() * sizeof(Pixel), source.data());
  auto history = storage.make(previous.size() * sizeof(Pixel), previous.data());
  auto headBuffer = storage.make(head.size() * sizeof(Pixel), head.data());
  auto destination = storage.make(pixels * sizeof(Pixel));
  auto nextHistory = storage.make(pixels * sizeof(Pixel));
  auto features = storage.make(fullWidth * fullHeight * 16 * sizeof(float));
  struct Params {
    uint32_t fullWidth,fullHeight,width,height,seed,historyValid,autoMask,style;
    float tone,structure,skin,paperWhite,intensity,blendScale,historyStrength,enabled,colorStrength,maxRatio;
  } params{fullWidth,fullHeight,width,height,19,0,1,0,.25f,.75f,-1,2,1,1,1,0,1,4};
  static_assert(sizeof(Params) == 72);
  vk::SpecConstants noSpec;
  auto pre = c.createComputePipeline(c.loadShaderModule((gameShaders / "game_preprocess.spv").string()), noSpec, "test game preprocess", 0);
  auto composite = c.createComputePipeline(c.loadShaderModule((gameShaders / "game_composite.spv").string()), noSpec, "test game composite", 0);
  const vk::Buffer* bindings[vk::kGenericBindings]{};
  bindings[0]=&in; bindings[1]=&history; bindings[2]=&headBuffer; bindings[3]=&destination;
  bindings[4]=&nextHistory; bindings[7]=&features;
  auto dispatch = [&](vk::Pipeline pipeline) {
    auto commands=c.beginCommands(); auto set=c.allocateSet(bindings);
    vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);
    vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_COMPUTE,c.pipelineLayout(),0,1,&set,0,nullptr);
    vkCmdPushConstants(commands,c.pipelineLayout(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
    vkCmdDispatch(commands,1,1,1); c.endAndSubmit(commands,true);
  };
  auto proxy = [](float value) {
    value *= 1.5f;
    if(value>.75f)value=.75f+.25f*(1-std::exp(-5.770780f*(value-.75f)));
    value=std::clamp(value,0.0f,1.0f);
    return num::roundF16(value<=.0031308f?12.92f*value:1.055f*std::pow(value,1.0f/2.4f)-.055f);
  };
  auto pixelValues = [&](const vk::Buffer& buffer) {
    auto bytes=c.download(buffer,pixels*sizeof(Pixel)); std::vector<Pixel> values(pixels);
    memcpy(values.data(),bytes.data(),bytes.size()); return values;
  };
  dispatch(pre);
  auto featureBytes=c.download(features,fullWidth*fullHeight*16*sizeof(float));
  std::vector<float> featureValues(featureBytes.size()/4);memcpy(featureValues.data(),featureBytes.data(),featureBytes.size());
  for(uint32_t y=0;y<fullHeight;y++)for(uint32_t x=0;x<fullWidth;x++) {
    const uint32_t base=(y*fullWidth+x)*16;
    for(uint32_t i=0;i<16;i++)require(std::isfinite(featureValues[base+i]),"nonfinite game feature");
    require(featureValues[base+3]==1&&featureValues[base+10]==0&&featureValues[base+11]==.25f&&
            featureValues[base+12]==1&&featureValues[base+13]==.75f&&featureValues[base+14]==.75f&&featureValues[base+15]==0,
            "game feature control/layout mismatch");
    for(uint32_t channel=0;channel<3;channel++)require(num::f32Bits(featureValues[base+4+channel])==num::f32Bits(featureValues[base+7+channel]),"reset features did not prime history from proxy");
    int sx=x<width?int(x):2*int(width)-int(x)-2,sy=y<height?int(y):2*int(height)-int(y)-2;
    sx=std::clamp(sx,0,int(width)-1);sy=std::clamp(sy,0,int(height)-1);
    for(uint32_t channel=0;channel<3;channel++) {
      float expected=num::roundF16(num::roundF16(proxy(source[2*(sy*width+sx)].rgb(channel))-.5f)*.125f);
      require(std::fabs(featureValues[base+4+channel]-expected)<=.000125f,"centered/reflected game proxy feature mismatch");
    }
  }
  dispatch(composite);
  auto out=pixelValues(destination),hist=pixelValues(nextHistory);
  for(uint32_t i=0;i<pixels;i++) {
    require(memcmp(&out[i],&source[i*2],sizeof(Pixel))==0,"disabled game output did not preserve original bits/alpha");
    for(uint32_t channel=0;channel<3;channel++)require(std::fabs(hist[i].rgb(channel)-proxy(source[i*2].rgb(channel)))<=.000977f,"disabled game history was not proxy");
    require(hist[i].a==1,"history alpha publication mismatch");
  }
  params.enabled=1;dispatch(composite);out=pixelValues(destination);
  for(uint32_t i=0;i<pixels;i++) {
    require(num::f32Bits(out[i].a)==num::f32Bits(source[i*2].a),"enabled game output alpha changed");
    for(uint32_t channel=0;channel<3;channel++) {
      float expected=source[i*2].rgb(channel),actual=out[i].rgb(channel);
      require(std::isfinite(actual)&&std::fabs(actual-expected)<=std::max(.0001f,std::fabs(expected)*.001f),"zero-head game output failed exposure-domain/HDR preservation");
    }
    if(i%2==0)require(out[i].b>1,"scene-linear HDR was clamped or display encoded");
  }
  params.historyValid=1;dispatch(pre);
  featureBytes=c.download(features,fullWidth*fullHeight*16*sizeof(float));memcpy(featureValues.data(),featureBytes.data(),featureBytes.size());
  for(uint32_t y=0;y<height;y++)for(uint32_t x=0;x<width;x++) {
    uint32_t base=(y*fullWidth+x)*16;
    require(featureValues[base+7]==-.03125f&&featureValues[base+8]==0&&featureValues[base+9]==.03125f,"temporal preprocess history centering/layout mismatch");
  }
  dispatch(composite);hist=pixelValues(nextHistory);
  for(uint32_t i=0;i<pixels;i++)for(uint32_t channel=0;channel<3;channel++) {
    float code=proxy(source[i*2].rgb(channel));
    float neural=std::clamp(std::fma(0.0f,.03125f,std::fma(code,.125f,-.0625f))*8+.5f,0.0f,1.0f);
    float blended=std::fma(.5f,previous[i].rgb(channel)-neural,neural);
    // These positive test values are normal halves, so truncation is the
    // explicit removal of the low 13 f32 mantissa bits.
    float expected=num::f32FromBits(num::f32Bits(blended)&~0x1fffu);
    require(std::fabs(hist[i].rgb(channel)-expected)<=.000977f,"temporal game history publication mismatch");
  }
  c.destroyPipeline(pre);c.destroyPipeline(composite);
  printf("selftest game feature/reset/HDR/alpha/history: PASS (model-free, numerical tolerances for transcendentals)\n");
}
std::filesystem::path syntheticGraphModel(const std::string& shaderDirectory) {
  // Generated sparse weights exercise every original graph tensor lookup and
  // dispatch. This fixture is not extracted from, or intended to approximate,
  // the NVIDIA model. Keep it under the build directory for inspection.
  auto root=std::filesystem::absolute(std::filesystem::path(shaderDirectory).parent_path()/
      ("synthetic-graph-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
  std::filesystem::create_directories(root/"model");
  std::vector<uint8_t> stage;
  std::ostringstream entries; bool first=true;
  auto append=[&](int block,int layer,const std::vector<uint8_t>& bytes,const std::string& parameter="layer") {
    if(!first)entries<<",";first=false;
    entries<<"{\"name\":\"block"<<block<<".layer"<<layer<<"."<<parameter<<"\",\"block\":"<<block<<",\"layer\":"<<layer
           <<",\"parameter\":\""<<parameter<<"\",\"stage\":\"synthetic\",\"stageOffset\":"<<stage.size()<<",\"byteLength\":"<<bytes.size()<<"}";
    stage.insert(stage.end(),bytes.begin(),bytes.end());
  };
  auto half=[&](std::vector<uint8_t>& b,uint32_t offset,uint32_t count,float value=1) {
    require(uint64_t(offset)+count*2<=b.size(),"synthetic half aux exceeds tensor");
    uint16_t bits=num::f16Bits(value);for(uint32_t i=0;i<count;i++)memcpy(b.data()+offset+i*2,&bits,2);
  };
  auto scale=[&](std::vector<uint8_t>& b,uint32_t offset,uint32_t count) {
    require(uint64_t(offset)+count*4<=b.size(),"synthetic float aux exceeds tensor");
    float value=1;for(uint32_t i=0;i<count;i++)memcpy(b.data()+offset+i*4,&value,4);
  };
  auto fp8=[&](std::vector<uint8_t>& b,uint32_t offset,uint32_t K,uint32_t N,uint32_t batchK=0,uint8_t code=0x28) {
    if(!batchK)batchK=K;
    require(uint64_t(offset)+uint64_t(K)*N<=b.size(),"synthetic FP8 matrix exceeds tensor");
    for(uint32_t batch=0;batch<K/batchK;batch++)for(uint32_t n=0;n<N;n++) {
      uint32_t k=batch*batchK+nr::inversePackedInputIndex(n%batchK);
      uint32_t index=offset+nr::packedWeightIndex(k,n,N);
      require(index<b.size(),"synthetic FP8 packed index exceeds tensor");b[index]=code;
    }
  };
  auto fp16=[&](std::vector<uint8_t>& b,uint32_t offset,uint32_t K,uint32_t N,float value) {
    uint32_t paddedN=(N+15)&~15u;uint16_t bits=num::f16Bits(value);
    for(uint32_t n=0;n<N;n++) {
      uint32_t k=n%K,tile=(k/16)*(paddedN/16)+n/16,kk=k%16,nn=n%16;
      uint32_t lane=(nn%8)*4+(kk%8)/2,fragment=(kk>=8?2:0)+kk%2;
      uint32_t index=offset+(tile*256+lane*8+(nn/8)*4+fragment)*2;
      require(index+2<=b.size(),"synthetic f16 packed index exceeds tensor");memcpy(b.data()+index,&bits,2);
    }
  };
  auto fused=[&](int block,uint32_t C,nr::FusedLayout layout,uint32_t extra=16) {
    std::vector<uint8_t> b(layout.endWithoutPadding+extra);
    if(layout.expertFfn) {
      uint32_t e=C/32,w2=layout.expand+e*C*128,w3=w2+e*128*32;
      fp8(b,layout.expand,e*C,128,C);fp8(b,w2,e*128,32,128);fp8(b,w3,C,C);
    } else {fp8(b,layout.expand,C,128);fp8(b,layout.contractWeights,128,C);}
    fp8(b,layout.qkv,C,C*3);fp8(b,layout.projection,C,C);
    half(b,layout.ffnCosSkip,C);half(b,layout.attnCosSkip,C);scale(b,layout.scale,C/32);
    if(block==0)fp16(b,layout.inputAdapter,16,32,.5f);
    if(block==70){half(b,layout.inputScale,32);half(b,layout.adapterScale,32);fp16(b,layout.postWeights,32,4,.125f);}
    if(layout.upsampleWeight){fp8(b,layout.upsampleWeight,C*2,C,0,0x38);half(b,layout.transitionScale,C);}
    if(extra>16)fp8(b,layout.endWithoutPadding,C,C*2,0,0x38);
    append(block,0,b);
  };
  auto split=[&](int block,bool transition) {
    std::vector<uint8_t> branch(512*512+8*64*256+8*256*64);
    fp8(branch,0,512,512);fp8(branch,512*512,8*64,256,64);fp8(branch,512*512+8*64*256,8*256,64,256);append(block,0,branch);
    std::vector<uint8_t> contract(512*512+512*2);fp8(contract,0,512,512);half(contract,512*512,512);append(block,1,contract);
    std::vector<uint8_t> qkv(512*1536+16*8192+16*4);fp8(qkv,0,512,1536);scale(qkv,512*1536+16*8192,16);append(block,2,qkv);
    std::vector<uint8_t> projection(512*512+512*2);fp8(projection,0,512,512);half(projection,512*512,512);append(block,3,projection);
    if(transition){std::vector<uint8_t> t(512*1024);fp8(t,0,512,1024,0,0x38);append(block,4,t);}
  };
  fused(0,32,nr::preFusedLayout());
  for(int block=1;block<=22;block++) {
    uint32_t C=block<=4?32:block<=8?64:block<=14?128:256;
    uint32_t extra=block==4||block==8||block==14||block==22?C*C*2+16:16;
    fused(block,C,nr::fusedLayout(C),extra);
  }
  for(int block=23;block<=30;block++)split(block,block==30);
  for(int block=31;block<=38;block++) {
    std::vector<uint8_t> e(1024*4096);fp8(e,0,1024,4096);append(block,0,e);
    std::vector<uint8_t> contract(4096*1024+1024*2);fp8(contract,0,4096,1024);half(contract,4096*1024,1024);append(block,1,contract);
    std::vector<uint8_t> q(32*4+1024*3072);scale(q,0,32);fp8(q,32*4,1024,3072);append(block,2,q);
    std::vector<uint8_t> p(1024*1024+1024*2);fp8(p,0,1024,1024);half(p,1024*1024,1024);append(block,4,p);
  }
  {std::vector<uint8_t> b(1024*512+512*2);fp8(b,0,1024,512,0,0x38);half(b,1024*512,512);append(39,0,b);}
  for(int block=40;block<=47;block++)split(block,false);
  for(int block=48;block<=69;block++) {
    uint32_t C=block<=55?256:block<=61?128:block<=65?64:32;
    bool transition=block==48||block==56||block==62||block==66;
    fused(block,C,transition?nr::upsampleFusedLayout(C*2,C):nr::fusedLayout(C));
  }
  fused(70,32,nr::postFusedLayout(),0);
  {std::vector<uint8_t> blend(2);half(blend,0,1);append(70,0,blend,"blend_scale");}
  std::ofstream binary(root/"model"/"generated.bin",std::ios::binary);
  binary.write(reinterpret_cast<const char*>(stage.data()),std::streamsize(stage.size()));binary.close();
  require(bool(binary),"cannot write generated synthetic graph tensor file");
  const std::string digest=sha256Hex(stage.data(),stage.size());
  std::ofstream manifest(root/"manifest.json");
  manifest<<"{\"synthetic\":true,\"totals\":{\"blockCount\":71},\"stages\":[{\"id\":\"synthetic\",\"file\":\"generated.bin\",\"packedByteLength\":"
          <<stage.size()<<",\"sha256\":\""<<digest<<"\"}],\"tensors\":["<<entries.str()<<"]}";manifest.close();
  require(bool(manifest),"cannot write generated synthetic graph manifest");
  printf("selftest generated sparse synthetic graph model: %s (%llu bytes, no NVIDIA assets)\n",root.string().c_str(),(unsigned long long)stage.size());
  return root;
}
void completeGraphCase(vk::Context& c,nr::Kernels& kernels,const std::string& shaderDirectory) {
  auto directory=syntheticGraphModel(shaderDirectory);
  nr::Model model(c,directory.string(),true);
  auto geometry=nr::Geometry::fromValid(320,320);
  Buffers storage{c};std::vector<float> inputs(geometry.fullWidth*geometry.fullHeight*16);
  for(uint32_t y=0;y<geometry.fullHeight;y++)for(uint32_t x=0;x<geometry.fullWidth;x++)for(uint32_t ch=0;ch<16;ch++)
    inputs[(y*geometry.fullWidth+x)*16+ch]=float(int((x*7+y*13+ch*17)%97)-48)/128.0f;
  auto input=storage.activation(geometry.fullWidth*geometry.fullHeight,16,nr::Format::F32,inputs.data());input.label="synthetic features";
  std::vector<uint8_t> baseline;uint64_t separateBytes=0,reusedBytes=0;uint32_t separateCount=0,reusedCount=0,dispatches=0;
  auto run=[&](nr::Graph& graph) {
    auto commands=c.beginCommands();graph.record(commands,input);c.endAndSubmit(commands,true);
    auto bytes=c.download(graph.head().buffer,graph.head().validBytes());uint32_t nonzero=0;
    for(size_t i=0;i<bytes.size();i+=4){float value;memcpy(&value,bytes.data()+i,4);require(std::isfinite(value),"nonfinite synthetic graph head");if(value!=0)++nonzero;}
    require(nonzero>0,"synthetic graph fixture produced only zero outputs");
    dispatches=kernels.dispatchCount();printf("selftest generated graph head: %u nonzero f32 samples, %u dispatches\n",nonzero,dispatches);return bytes;
  };
  {
    nr::Graph::Options options;options.fusedBlocks=false;options.reuseScratch=false;
    nr::Graph graph(c,model,kernels,geometry,options);baseline=run(graph);separateBytes=graph.activationBytes();separateCount=graph.activationCount();
  }
  {
    nr::Graph::Options options;options.fusedBlocks=false;options.reuseScratch=true;
    nr::Graph graph(c,model,kernels,geometry,options);auto reused=run(graph);
    require(reused==baseline,"scratch-reused synthetic graph head differs from separate allocations");
    reusedBytes=graph.activationBytes();reusedCount=graph.activationCount();auto repeated=run(graph);
    require(repeated==baseline,"re-recorded synthetic graph head differs");
    require(graph.activationBytes()==reusedBytes&&graph.activationCount()==reusedCount,"re-recording allocated additional graph buffers");
  }
  require(reusedBytes<separateBytes&&reusedCount<separateCount,"native graph scratch reuse saved no memory");
  printf("selftest synthetic 320 graph scratch/re-record: PASS (same backend exact head; separate %u buffers %.3f MiB, reused %u buffers %.3f MiB; inputs excluded)\n",
    separateCount,separateBytes/1048576.0,reusedCount,reusedBytes/1048576.0);
}
void gemmCase(vk::Context& c, nr::Kernels& kernels, uint32_t K, uint32_t N, uint32_t batches,
              bool broadcast, uint32_t partition, bool residualHalf, bool silu, bool dual, uint32_t outputBase) {
  Buffers storage{c};
  const uint32_t rows = 67, padded = nr::alignRows(rows);
  const uint32_t inputStride = 16 + K * (broadcast ? 1 : batches), outputStride = outputBase + batches * N;
  std::vector<uint8_t> input((size_t)padded * inputStride), weights((size_t)batches * K * N);
  for (auto& x : input) x = randomE4(); for (auto& x : weights) x = randomE4();
  std::vector<uint8_t> residual8((size_t)padded * outputStride);
  std::vector<uint16_t> residual16(residual8.size()), scales(N);
  for (auto& x : residual8) x = randomE4(); for (auto& x : residual16) x = randomHalf();
  for (auto& x : scales) x = num::f16Bits(0.5f);
  auto a = storage.activation(rows, inputStride, nr::Format::E4, input.data());
  auto out = storage.activation(rows, outputStride, dual ? nr::Format::F16 : nr::Format::E4);
  auto outDual = storage.activation(rows, outputStride, nr::Format::E4);
  auto residual = residualHalf ? storage.activation(rows, outputStride, nr::Format::F16, residual16.data())
                               : storage.activation(rows, outputStride, nr::Format::E4, residual8.data());
  nr::Tensor aux; aux.raw = storage.make(scales.size() * 2, scales.data());
  auto w = storage.make(weights.size(), weights.data());
  nr::GemmFp8Args args;
  args.input = &a; args.inputColumnBase = 16; args.weights = &w; args.Nmatrix = N;
  args.output = &out; args.outputColumnOffset = outputBase; args.rows = rows; args.K = K; args.N = N;
  args.batches = batches; args.broadcastInput = broadcast; args.partition = partition;
  args.residual = &residual; args.scaleResidual = true; args.auxTensor = &aux; args.silu = silu;
  args.quantize = !dual; args.dualOutput = dual ? &outDual : nullptr;
  auto commands = c.beginCommands(); kernels.gemmFp8(commands, args); c.endAndSubmit(commands, true);
  auto bytes = c.download(out.buffer, out.validBytes());
  auto dualBytes = dual ? c.download(outDual.buffer, outDual.validBytes()) : std::vector<uint8_t>();
  Comparison compare{c.isReference()};
  for (uint32_t r = 0; r < rows; ++r) for (uint32_t batch = 0; batch < batches; ++batch) for (uint32_t n = 0; n < N; ++n) {
    const size_t oi = (size_t)r * outputStride + outputBase + batch * N + n;
    float rv = residualHalf ? num::f16ToF32(residual16[oi]) : num::e4m3ToF32(residual8[oi]);
    float value = num::roundF16(rv * num::f16ToF32(scales[n])), total = 0;
    for (uint32_t kb = 0; kb < K; kb += 16) {
      float av[16], bv[16];
      for (uint32_t k = 0; k < 16; ++k) {
        av[k] = num::e4m3ToF32(input[(size_t)r * inputStride + 16 + (broadcast ? 0 : batch * K) + kb + k]);
        size_t wi = ((size_t)(batch * (K / 32) + (kb + k) / 32) * N + n) * 32 + (kb + k) % 32;
        bv[k] = num::e4m3ToF32(weights[wi]);
      }
      value = ref::adaFp8Fdpa16(av, bv, 16, value);
      if (partition && (kb + 16) % partition == 0) { total = kb < partition ? value : num::roundF16(total + value); value = 0; }
    }
    if (partition) value = total;
    if (silu) value = ref::mpCubicSilu(value);
    if (dual) {
      uint16_t actual; memcpy(&actual, bytes.data() + oi * 2, 2);
      compare.sample(num::f16ToF32(actual), value, actual == num::f16Bits(value));
      uint8_t expected = num::e4m3FromF32(value);
      compare.sample(num::e4m3ToF32(dualBytes[oi]), num::e4m3ToF32(expected), dualBytes[oi] == expected);
    } else {
      uint8_t expected = num::e4m3FromF32(value);
      compare.sample(num::e4m3ToF32(bytes[oi]), num::e4m3ToF32(expected), bytes[oi] == expected);
    }
  }
  const std::string label = "gemm K" + std::to_string(K) + " N" + std::to_string(N) + " batch" + std::to_string(batches);
  compare.finish(label.c_str());
}
void f16Case(vk::Context& c, nr::Kernels& kernels) {
  Buffers storage{c}; constexpr uint32_t rows = 67, K = 32, N = 4, paddedN = 16;
  std::vector<uint16_t> input(nr::alignRows(rows) * K), weights(K * paddedN);
  for (auto& x : input) x = randomHalf(); for (auto& x : weights) x = randomHalf();
  auto a = storage.activation(rows, K, nr::Format::F16, input.data());
  auto out = storage.activation(rows, N, nr::Format::F32); auto w = storage.make(weights.size() * 2, weights.data());
  nr::GemmF16Args args; args.input = &a; args.weights = &w; args.paddedN = paddedN; args.output = &out;
  args.rows = rows; args.K = K; args.N = N;
  auto commands = c.beginCommands(); kernels.gemmF16(commands, args); c.endAndSubmit(commands, true);
  auto bytes = c.download(out.buffer, out.validBytes()); Comparison compare{true};
  for (uint32_t r = 0; r < rows; ++r) for (uint32_t n = 0; n < N; ++n) {
    float value = 0;
    for (uint32_t kb = 0; kb < K; kb += 8) {
      float av[8], bv[8];
      for (uint32_t k = 0; k < 8; ++k) { av[k] = num::f16ToF32(input[r * K + kb + k]); bv[k] = num::f16ToF32(weights[(kb + k) * paddedN + n]); }
      value = ref::adaF16Fdpa8(av, bv, 8, value);
    }
    float actual; memcpy(&actual, bytes.data() + (r * N + n) * 4, 4);
    compare.sample(actual, value, num::f32Bits(actual) == num::f32Bits(value));
  }
  compare.finish("F24 half adapter/head");
}
void windowCase(vk::Context& c, nr::Kernels& kernels, uint32_t shift = 0) {
  Buffers storage{c}; constexpr uint32_t width = 8, height = 8, heads = 1, channels = 32;
  std::vector<uint8_t> normalized(64 * 96), rawPrior(8192, 0);
  std::vector<uint16_t> plainPrior(4096);
  for (uint32_t q = 0; q < 64; ++q) for (uint32_t key = 0; key < 64; ++key) {
    uint16_t value = num::f16Bits(float(int(randomWord() % 65) - 32) / 128.0f);
    plainPrior[q * 64 + key] = value;
    uint32_t qp = (q / 32) * 32 + ((q % 8) / 4) * 16 + ((q / 8) % 4) * 4 + q % 4;
    uint32_t m = qp % 16, n = key % 16, lane = (m % 8) * 4 + (n % 8) / 2;
    uint32_t index = (qp / 16) * 1024 + (key / 16) * 256 + lane * 8 + (n / 8) * 4 + (m >= 8 ? 2 : 0) + n % 2;
    memcpy(rawPrior.data() + index * 2, &value, 2);
  }
  std::vector<float> decoded(normalized.size());
  for (size_t i = 0; i < normalized.size(); ++i) { normalized[i] = randomE4(); decoded[i] = num::e4m3ToF32(normalized[i]); }
  auto a = storage.activation(64, 96, nr::Format::E4, normalized.data());
  auto out = storage.activation(64, channels, nr::Format::E4); auto prior = storage.make(plainPrior.size() * 2, plainPrior.data());
  auto commands = c.beginCommands(); kernels.windowAttend(commands, a, prior, out, width, height, heads, shift, shift); c.endAndSubmit(commands, true);
  auto bytes = c.download(out.buffer, out.validBytes());
  nr::Tensor tensor; tensor.bytes = rawPrior.data(); tensor.byteLength = uint32_t(rawPrior.size());
  std::vector<float> expected(64 * 32);
  Comparison compare{c.isReference()};
  const uint32_t windowCount = (width + shift + 7) / 8;
  for (uint32_t wy = 0; wy < windowCount; ++wy) for (uint32_t wx = 0; wx < windowCount; ++wx) {
    const int originX = int(wx * 8) - int(shift), originY = int(wy * 8) - int(shift);
    ref::windowAttendRef(decoded, width, height, channels, 0, originX, originY, tensor, 0, expected.data());
    for (uint32_t q = 0; q < 64; ++q) {
      int x = originX + int(q % 8), y = originY + int(q / 8);
      if (x < 0 || y < 0 || x >= int(width) || y >= int(height)) continue;
      for (uint32_t column = 0; column < 32; ++column) {
        float actual = num::e4m3ToF32(bytes[(y * width + x) * channels + column]);
        float value = expected[q * 32 + column];
        // CPU window oracle returns decoded numbers, so signed zeros compare as values.
        compare.sample(actual, value, actual == value);
      }
    }
  }
  compare.finish(shift ? "shifted/OOB window" : "64-key window/prior");
}
void normalizeCase(vk::Context& c, nr::Kernels& kernels) {
  Buffers storage{c}; constexpr uint32_t tokens = 67, heads = 2, stride = heads * 96;
  std::vector<uint16_t> input(nr::alignRows(tokens) * stride);
  std::vector<float> decoded(input.size());
  for (size_t i = 0; i < input.size(); ++i) { input[i] = i < stride ? 0 : randomHalf(); decoded[i] = num::f16ToF32(input[i]); }
  float scales[heads] = {1.25f, 0.75f};
  nr::Tensor tensor; tensor.raw = storage.make(sizeof(scales), scales);
  auto a = storage.activation(tokens, stride, nr::Format::F16, input.data());
  auto out = storage.activation(tokens, stride, nr::Format::E4);
  auto commands = c.beginCommands(); kernels.windowNormalize(commands, a, tensor, 0, out, tokens, heads); c.endAndSubmit(commands, true);
  auto bytes = c.download(out.buffer, out.validBytes()); Comparison compare{true};
  for (uint32_t token = 0; token < tokens; ++token) for (uint32_t head = 0; head < heads; ++head) {
    float expected[96]; ref::windowNormalizeRef(decoded.data() + token * stride, head, scales[head], expected);
    for (uint32_t i = 0; i < 96; ++i) {
      uint8_t actual = bytes[token * stride + head * 96 + i];
      compare.sample(num::e4m3ToF32(actual), expected[i], num::e4m3ToF32(actual) == expected[i]);
    }
  }
  compare.finish("cosine norm + zero/NaN");
}
void globalCase(vk::Context& c, nr::Kernels& kernels) {
  Buffers storage{c}; constexpr uint32_t tokens = 65, padded = 128, heads = 2, channels = heads * 32, stride = channels * 3;
  std::vector<uint8_t> input(padded * stride, 0);
  for (uint32_t i = 0; i < tokens * stride; ++i) input[i] = randomE4();
  auto a = storage.activation(padded, stride, nr::Format::E4, input.data());
  auto out = storage.activation(tokens, channels, nr::Format::E4);
  auto commands = c.beginCommands(); kernels.globalAttend(commands, a, out, tokens, padded, heads); c.endAndSubmit(commands, true);
  auto bytes = c.download(out.buffer, out.validBytes()); Comparison compare{c.isReference()};
  auto h = [](float v) { return num::roundF16(v); };
  auto exp = [&](float v) {
    float affine = std::clamp(h(std::fma(v, 0.08953857421875f, 1.708984375f)), 1.439453125f, 1.9775390625f);
    return num::f16ToF32(uint16_t(((uint32_t(num::f16Bits(affine)) << 4) + 0x4000) & 0xffff));
  };
  for (uint32_t q = 0; q < tokens; ++q) for (uint32_t head = 0; head < heads; ++head) {
    float scores[padded]; uint8_t weights[padded];
    for (uint32_t key = 0; key < padded; ++key) {
      float value = 0;
      for (uint32_t kb = 0; kb < 32; kb += 16) {
        float av[16], bv[16];
        for (uint32_t i = 0; i < 16; ++i) {
          av[i] = num::e4m3ToF32(input[q * stride + head * 96 + kb + i]);
          bv[i] = key < tokens ? num::e4m3ToF32(input[key * stride + head * 96 + 32 + kb + i]) : 0;
        }
        value = ref::adaFp8Fdpa16(av, bv, 16, value);
      }
      scores[key] = exp(value); weights[key] = num::e4m3FromF32(scores[key]);
    }
    auto pair = [&](uint32_t base, uint32_t p, uint32_t parity) {
      uint32_t k = base + p * 2 + parity;
      float a0 = h(scores[k] + scores[k + 8]), a1 = h(scores[k + 16] + scores[k + 24]);
      float a2 = h(scores[k + 32] + scores[k + 40]), a3 = h(scores[k + 48] + scores[k + 56]);
      return h(h(h(a0 + a1) + a2) + a3);
    };
    float total = 0;
    for (uint32_t base = 0; base < padded; base += 64) {
      float even = h(h(h(pair(base, 0, 0) + pair(base, 1, 0)) + pair(base, 2, 0)) + pair(base, 3, 0));
      float odd = h(h(h(pair(base, 0, 1) + pair(base, 1, 1)) + pair(base, 2, 1)) + pair(base, 3, 1));
      total = h(total + h(even + odd));
    }
    total = h(total - h(exp(0) * float(padded - tokens)));
    float reciprocal = h(1.0f / total);
    for (uint32_t column = 0; column < 32; ++column) {
      float value = 0;
      for (uint32_t kb = 0; kb < padded; kb += 16) {
        float av[16], bv[16];
        for (uint32_t i = 0; i < 16; ++i) {
          av[i] = num::e4m3ToF32(weights[kb + i]);
          bv[i] = kb + i < tokens ? num::e4m3ToF32(input[(kb + i) * stride + head * 96 + 64 + column]) : 0;
        }
        value = ref::adaFp8Fdpa16(av, bv, 16, value);
      }
      uint8_t expected = num::e4m3FromF32(h(value * reciprocal));
      uint8_t actual = bytes[q * channels + head * 32 + column];
      compare.sample(num::e4m3ToF32(actual), num::e4m3ToF32(expected), actual == expected);
    }
  }
  compare.finish("ViT 65+63 padding keys");
}
}

int runNativeSelfTest(vk::Backend backend, const std::string& shaderDirectory) {
  try {
    vk::Context context(backend);
    require(context.backend() != vk::Backend::Nvidia, "selftest selects --backend reference or amd");
    nr::Kernels kernels(context, shaderDirectory);
    publicationCase(context, kernels);
    gemmCase(context, kernels, 64, 32, 2, false, 0, false, false, false, 0);
    gemmCase(context, kernels, 128, 32, 1, false, 64, true, true, false, 0);
    gemmCase(context, kernels, 32, 16, 3, true, 0, false, false, false, 16);
    gemmCase(context, kernels, 256, 64, 1, false, 64, false, false, true, 0);
    f16Case(context, kernels); normalizeCase(context, kernels);
    windowCase(context, kernels); windowCase(context, kernels, 4); globalCase(context, kernels);
    gameFrameCase(context,shaderDirectory);
    if(getenv("OPEN_NR_GRAPH_SELFTEST"))completeGraphCase(context,kernels,shaderDirectory);
    require(vk::Context::validationErrors() == 0, "Vulkan validation reported errors");
    printf("NATIVE SELFTEST PASS (%s)\n", vk::backendName(context.backend()));
    return 0;
  } catch (const std::exception& e) { fprintf(stderr, "NATIVE SELFTEST FAIL: %s\n", e.what()); return 1; }
}
