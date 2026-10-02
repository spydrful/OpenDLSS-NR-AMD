#pragma once
// CPU-only selection lifetime and tuning-policy validation. GPU modules and
// evidence identities are validated by Kernels after this prospective policy.
#include "amd_config.h"
#include "amd_qualified_fallback.h"
#include "json.h"

namespace amd {
inline bool diagnosticSelection(const Options& options) {
  return options.kernels == KernelMode::Optimized || options.arithmetic != Arithmetic::K16 ||
      options.tileN != 16 || options.stageK != 16 || options.windowQueries != 64 ||
      options.fusion || options.expertFusion || options.blockFusion || options.hardwarePublication;
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
  number("tile_n",policy.tileN);number("stage_k",policy.stageK);number("window_queries",policy.windowQueries);
  flag("fusion",policy.fusion);flag("expert_fusion",policy.expertFusion);
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
      require(variant.string == "amd_gemm_optimized","AMD tuning GEMM variant mismatch");
      geometry(policy.tileN,policy.stageK);
    } else if(family.string == "window_attention") {
      require(variant.string == (policy.windowQueries==64 ? "amd_window_optimized" : "amd_window_small"),
              "AMD tuning attention variant mismatch");
      geometry(64,16); // 64 keys and ordered K16 matrix steps, not the GEMM tile.
      const auto& queries=record["window_queries"];
      require(queries.kind == json::Value::Number && queries.number == policy.windowQueries,
              "AMD tuning attention query specialization mismatch");
    } else if(family.string == "ffn") {
      require(policy.fusion && variant.string == "amd_ffn32","AMD tuning FFN capability mismatch");geometry(32,32);
    } else if(family.string == "qkv_attention") {
      require(policy.fusion && variant.string == "amd_qkv32","AMD tuning QKV capability mismatch");geometry(64,16);
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
  const bool fusion=flag("fusion"),expert=flag("expert_fusion"),block=flag("block_fusion"),hardware=flag("hardware_publication");
  if(allowAutoSelection){
    prospective.tileN=tileN;prospective.stageK=stageK;prospective.windowQueries=queries;
    prospective.fusion=fusion;prospective.expertFusion=expert;prospective.blockFusion=block;prospective.hardwarePublication=hardware;
  }else{
    for(const char* key:{"fusion","expert_fusion","block_fusion","hardware_publication"})
      require(selected.has(key),"forced AMD tuning capability is missing");
    require(tileN == current.tileN && stageK == current.stageK && queries == current.windowQueries,
            "forced AMD tuning policy mismatch");
    require(fusion == current.fusion && expert == current.expertFusion && block == current.blockFusion && hardware == current.hardwarePublication,
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
