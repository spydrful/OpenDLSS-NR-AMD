#pragma once
// CPU-only selection lifetime and tuning-policy validation. GPU modules and
// evidence identities are validated by Kernels after this prospective policy.
#include "amd_config.h"
#include "amd_qualified_fallback.h"
#include "json.h"

namespace amd {
inline bool diagnosticSelection(const Options& options) {
  return options.kernels == KernelMode::Optimized || options.arithmetic != Arithmetic::K16 || options.gemm != Gemm::Shared ||
      options.tileN != 16 || options.stageK != 16 || options.windowQueries != 64 || options.windowLayout != WindowLayout::Staged ||
      options.ffn32Enabled() || options.qkv32Enabled() || options.expertFusion || options.blockFusion || options.hardwarePublication;
}
class Selection {
 public:
  explicit Selection(const Options& requested) : requested_(requested), current_(requested) {}
  // A prior geometry's tuned policy is never the starting point for another
  // graph or for revalidating a replaced/invalid tuning file.
  void restart() { current_ = requested_; }
  Options& current() { return current_; }
  bool forced() const { return requested_.kernels != KernelMode::Auto || diagnosticSelection(requested_); }
 private:
  const Options requested_;
  Options current_;
};
inline bool pinnedFallbackIdentity(const std::string& device, const std::string& driver,
                                   const std::string& model, const std::string& baseline) {
  return device == qualified::device && driver == qualified::driver &&
      model == qualified::model && baseline == qualified::baseline;
}
// Shader identities name SPIR-V bytes, not specialization constants. Bind the
// complete measured session policy to each qualified operator's evidence so an
// edited default cannot inherit another tile/query/publication qualification.
struct Fusion32Policy { bool ffn, qkv; };
inline Gemm gemmPolicy(const json::Value& value) {
  if(!value.has("gemm"))return Gemm::Shared; // Immutable legacy records only name shared GEMM.
  if(value["gemm"].kind != json::Value::String)throw std::runtime_error("AMD tuning GEMM policy must be a string");
  return Options::parseGemm(value["gemm"].string);
}
inline WindowLayout windowLayoutPolicy(const json::Value& value) {
  if(!value.has("window_layout"))return WindowLayout::Staged; // Immutable prior records only name staged attention.
  if(value["window_layout"].kind != json::Value::String)throw std::runtime_error("AMD tuning window layout must be a string");
  return Options::parseWindowLayout(value["window_layout"].string);
}
inline Fusion32Policy fusion32Policy(const json::Value& value) {
  if(value["fusion"].kind != json::Value::Bool)
    throw std::runtime_error("AMD tuning legacy fusion summary must be Boolean");
  const bool legacy=value["fusion"].boolean;
  const bool ffn=value.has("ffn32_fusion"),qkv=value.has("qkv32_fusion");
  if(ffn != qkv)throw std::runtime_error("AMD tuning independent fusion policy requires both routes");
  if(!ffn)return {legacy,legacy}; // Existing immutable caches name the shorthand.
  const auto& f=value["ffn32_fusion"];const auto& q=value["qkv32_fusion"];
  if(f.kind != json::Value::Bool || q.kind != json::Value::Bool || legacy != (f.boolean && q.boolean))
    throw std::runtime_error("AMD tuning independent fusion policy/summary mismatch");
  return {f.boolean,q.boolean};
}
inline void requireTuningPolicyMatch(const json::Value& value, const Options& policy) {
  auto require=[](bool valid,const char* message){if(!valid)throw std::runtime_error(message);};
  require(value.kind == json::Value::Object,"AMD tuning evidence policy must be an object");
  auto text=[&](const char* key,const char* expected){const auto& v=value[key];
    require(v.kind == json::Value::String && v.string == expected,"AMD tuning evidence policy mismatch");};
  auto number=[&](const char* key,uint32_t expected){const auto& v=value[key];
    require(v.kind == json::Value::Number && v.number == expected,"AMD tuning evidence specialization mismatch");};
  auto flag=[&](const char* key,bool expected){const auto& v=value[key];
    require(v.kind == json::Value::Bool && v.boolean == expected,"AMD tuning evidence capability mismatch");};
  text("kernels","optimized");text("arithmetic","k16");
  require(gemmPolicy(value)==policy.gemm,"AMD tuning GEMM evidence policy mismatch");
  require(windowLayoutPolicy(value)==policy.windowLayout,"AMD tuning window layout evidence policy mismatch");
  number("tile_n",policy.tileN);number("stage_k",policy.stageK);number("window_queries",policy.windowQueries);
  const auto fusion=fusion32Policy(value);
  require(fusion.ffn == policy.ffn32Enabled() && fusion.qkv == policy.qkv32Enabled(),
          "AMD tuning evidence independent fusion capability mismatch");
  flag("expert_fusion",policy.expertFusion);
  flag("block_fusion",policy.blockFusion);flag("hardware_publication",policy.hardwarePublication);
}
inline void validateTuningRecordPolicies(const json::Value& doc, const Options& policy) {
  auto require=[](bool valid,const char* message){if(!valid)throw std::runtime_error(message);};
  const auto& records=doc["records"];
  require(records.kind == json::Value::Array && !records.array.empty(),"AMD tuning contains no qualified operators");
  for(const auto& record:records.array) {
    require(record.kind == json::Value::Object,"AMD tuning record must be an object");
    requireTuningPolicyMatch(record["evidence"]["selected"],policy);
    const auto& family=record["key"]["family"];
    const auto& variant=record["variant"];
    require(family.kind == json::Value::String && variant.kind == json::Value::String,
            "invalid AMD tuning operator family/variant");
    auto geometry=[&](uint32_t tileN,uint32_t stageK){
      const auto& n=record["tile_n"];const auto& k=record["stage_k"];
      require(n.kind == json::Value::Number && n.number == tileN &&
              k.kind == json::Value::Number && k.number == stageK,"AMD tuning operator geometry mismatch");};
    if(family.string == "fp8_gemm") {
      require(variant.string == policy.gemmShaderName(),"AMD tuning GEMM variant mismatch");
      geometry(policy.tileN,policy.stageK);
    } else if(family.string == "window_attention") {
      require(variant.string == policy.windowShaderName(),
              "AMD tuning attention variant mismatch");
      geometry(64,16); // 64 keys and ordered K16 matrix steps, not the GEMM tile.
      const auto& queries=record["window_queries"];
      require(queries.kind == json::Value::Number && queries.number == policy.windowQueries,
              "AMD tuning attention query specialization mismatch");
    } else if(family.string == "ffn") {
      require(policy.ffn32Enabled() && variant.string == "amd_ffn32","AMD tuning FFN capability mismatch");geometry(32,32);
    } else if(family.string == "qkv_attention") {
      require(policy.qkv32Enabled() && variant.string == "amd_qkv32","AMD tuning QKV capability mismatch");geometry(64,16);
    } else if(family.string == "expert_ffn") {
      require(policy.expertFusion && variant.string == "amd_expert_ffn","AMD tuning expert capability mismatch");geometry(128,32);
    } else if(family.string == "c32_block") {
      require(policy.blockFusion && variant.string == "amd_block32","AMD tuning C32 capability mismatch");geometry(64,16);
    } else throw std::runtime_error("unsupported AMD tuning operator family");
  }
}
inline Options tuningPolicy(const json::Value& doc, const Options& current,
                            uint32_t width, uint32_t height, bool allowAutoSelection) {
  auto require=[](bool valid,const char* message){if(!valid)throw std::runtime_error(message);};
  require(doc["format"].str() == "OpenNR-amd-tuning-v1", "invalid AMD tuning format");
  require(doc["optimized_default_eligible"].kind == json::Value::Bool && doc["optimized_default_eligible"].boolean,
          "AMD tuning combination lacks complete-inference qualification");
  const auto& selected=doc["default_selection"];
  require(selected["arithmetic"].str() == "k16" && selected["kernels"].str() == "optimized",
          "AMD auto tuning only accepts preserving arithmetic");
  require(current.arithmetic == Arithmetic::K16, "AMD tuning cannot qualify an experimental arithmetic override");
  auto tile=[&](const char* key){const auto& v=selected[key];
    require(v.kind == json::Value::Number && (v.number == 16 || v.number == 32 || v.number == 64),
            "invalid AMD tuning tile");return uint32_t(v.number);};
  Options prospective=current;
  const uint32_t tileN=tile("tile_n"),stageK=tile("stage_k");
  uint32_t queries=64;
  if(selected.has("window_queries")){const auto& v=selected["window_queries"];
    require(v.kind == json::Value::Number && (v.number == 16 || v.number == 32 || v.number == 64),
            "invalid AMD tuning query tile");queries=uint32_t(v.number);}
  auto flag=[&](const char* key){if(!selected.has(key))return false;
    require(selected[key].kind == json::Value::Bool,"invalid AMD tuning capability");return selected[key].boolean;};
  const auto fusion=fusion32Policy(selected);
  const auto gemm=gemmPolicy(selected);
  const auto windowLayout=windowLayoutPolicy(selected);
  require(windowLayout==WindowLayout::Staged || queries==16 || queries==32,"register AMD tuning window layout requires Q16 or Q32");
  require((gemm!=Gemm::Direct && gemm!=Gemm::DirectRte) || stageK==16,"direct AMD tuning GEMM requires stage_k=16");
  const bool expert=flag("expert_fusion"),block=flag("block_fusion"),hardware=flag("hardware_publication");
  require(gemm!=Gemm::DirectRte || !hardware,"scalar RTE tuning cannot name packed hardware publication");
  if(allowAutoSelection){
    prospective.tileN=tileN;prospective.stageK=stageK;prospective.windowQueries=queries;
    prospective.gemm=gemm;
    prospective.windowLayout=windowLayout;
    prospective.ffn32Fusion=fusion.ffn;prospective.qkv32Fusion=fusion.qkv;prospective.fusion=fusion.ffn && fusion.qkv;
    prospective.expertFusion=expert;prospective.blockFusion=block;prospective.hardwarePublication=hardware;
  }else{
    for(const char* key:{"fusion","expert_fusion","block_fusion","hardware_publication"})
      require(selected.has(key),"forced AMD tuning capability is missing");
    require(tileN == current.tileN && stageK == current.stageK && queries == current.windowQueries,
            "forced AMD tuning policy mismatch");
    require(gemm==current.gemm,"forced AMD GEMM policy mismatch");
    require(windowLayout==current.windowLayout,"forced AMD window layout policy mismatch");
    require(fusion.ffn == current.ffn32Enabled() && fusion.qkv == current.qkv32Enabled() && expert == current.expertFusion && block == current.blockFusion && hardware == current.hardwarePublication,
            "forced AMD capability policy mismatch");
  }
  const auto& geometry=doc["geometry"];
  require(geometry["padded_width"].kind == json::Value::Number && geometry["padded_height"].kind == json::Value::Number &&
      geometry["padded_width"].number == width && geometry["padded_height"].number == height,
      "AMD tuning geometry mismatch");
  requireTuningPolicyMatch(selected,prospective);
  return prospective;
}
} // namespace amd
