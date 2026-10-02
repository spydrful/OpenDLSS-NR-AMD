// CPU-only regression of the policy state and parser used by Kernels.
#include "../src/amd_selection.h"
#include <cstdio>
#include <functional>
#include <fstream>
#include <iterator>

namespace {
unsigned checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void rejects(const std::function<void()>& fn,const char* message){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}expect(rejected,message);}
json::Value record(){return json::parse(R"({
  "format":"OpenNR-amd-tuning-v1","optimized_default_eligible":true,
  "default_selection":{"kernels":"optimized","arithmetic":"k16","tile_n":16,"stage_k":16,"window_queries":32,
    "fusion":false,"expert_fusion":false,"block_fusion":false,"hardware_publication":false},
  "geometry":{"padded_width":1728,"padded_height":960}
})");}
json::Value& field(json::Value& doc,const char* name){return doc.object.at("default_selection").object.at(name);}
void number(json::Value& value,double n){value.kind=json::Value::Number;value.number=n;}
void boolean(json::Value& value,bool b){value.kind=json::Value::Bool;value.boolean=b;}
void routes(json::Value& selected,bool ffn,bool qkv){
  boolean(selected.object["fusion"],ffn&&qkv);
  boolean(selected.object["ffn32_fusion"],ffn);boolean(selected.object["qkv32_fusion"],qkv);
}
}
int main(int argc,char** argv){
  try{
    amd::Options requested;
    amd::Selection selection(requested);
    const auto tuning=record();
    selection.current()=amd::tuningPolicy(tuning,selection.current(),1728,960,!selection.forced());
    expect(selection.current().windowQueries==32,"valid target tuning did not select Q32");
    selection.current().tuningPath="automatically discovered tuning.json";

    // Resize after a successful Q32 selection: the prospective parser rejects
    // the stale geometry without mutating the newly established Q64 fallback.
    selection.restart();
    expect(selection.current().windowQueries==64 && selection.current().tuningPath.empty(),"resize inherited the previous geometry's tuning/path");
    rejects([&]{selection.current()=amd::tuningPolicy(tuning,selection.current(),384,256,!selection.forced());},"stale geometry accepted on resize");
    expect(selection.current().windowQueries==64,"failed resize qualification restored stale Q32");

    // The same geometry must also revalidate: corrupted or replaced tuning is
    // not permitted to retain a previously selected policy.
    selection.current()=amd::tuningPolicy(tuning,selection.current(),1728,960,true);
    selection.restart();
    auto corrupt=tuning;corrupt.object.at("optimized_default_eligible").boolean=false;
    rejects([&]{selection.current()=amd::tuningPolicy(corrupt,selection.current(),1728,960,true);},"corrupt tuning remained eligible");
    expect(selection.current().windowQueries==64,"invalid tuning retained prior Q32");
    selection.current()=amd::tuningPolicy(tuning,selection.current(),1728,960,true);
    expect(selection.current().windowQueries==32,"returning to qualified geometry did not reselect Q32");

    auto matches=[](const std::string& device,const std::string& driver,const std::string& model,const std::string& baseline){
      return amd::pinnedFallbackIdentity(device,driver,model,baseline);
    };
    expect(matches(amd::qualified::device,amd::qualified::driver,amd::qualified::model,amd::qualified::baseline),"pinned preserving identity rejected");
    selection.restart();
    expect(!matches(amd::qualified::device,amd::qualified::driver,"changed-model",amd::qualified::baseline) && selection.current().windowQueries==64,
           "model switch inherited a tuned policy or a pinned fallback");
    expect(!matches("1002:other",amd::qualified::driver,amd::qualified::model,amd::qualified::baseline),"changed GPU inherited pinned fallback");
    expect(!matches(amd::qualified::device,"changed-driver",amd::qualified::model,amd::qualified::baseline),"changed driver inherited pinned fallback");
    expect(!matches(amd::qualified::device,amd::qualified::driver,amd::qualified::model,"changed-baseline"),"changed shader baseline inherited pinned fallback");

    amd::Options forced;forced.kernels=amd::KernelMode::Optimized;forced.windowQueries=32;forced.tuningPath="explicit.json";
    amd::Selection diagnostic(forced);
    expect(diagnostic.forced(),"explicit optimized mode became automatic");
    diagnostic.current().windowQueries=16;diagnostic.restart();
    expect(diagnostic.current().windowQueries==32 && diagnostic.current().tuningPath=="explicit.json","restart replaced an explicit request");
    expect(amd::tuningPolicy(tuning,diagnostic.current(),1728,960,false).windowQueries==32,"matching explicit policy rejected");
    auto changed=tuning;number(field(changed,"window_queries"),16);
    rejects([&]{(void)amd::tuningPolicy(changed,diagnostic.current(),1728,960,false);},"explicit query specialization was silently overridden");
    expect(diagnostic.current().windowQueries==32,"forced failure changed explicit policy");
    forced.kernels=amd::KernelMode::Auto;forced.tileN=32;
    amd::Selection implicitDiagnostic(forced);
    expect(implicitDiagnostic.forced(),"explicit auto-mode tile override was treated as a qualified default");
    rejects([&]{(void)amd::tuningPolicy(tuning,implicitDiagnostic.current(),1728,960,false);},"packaged tuning replaced an explicit tile override");
    for(auto arithmetic:{amd::Arithmetic::K32,amd::Arithmetic::Final}){
      auto experimental=forced;experimental.arithmetic=arithmetic;
      amd::Selection mode(experimental);
      expect(mode.forced(),"experimental arithmetic became automatic");
      rejects([&]{(void)amd::tuningPolicy(tuning,mode.current(),1728,960,false);},"K16 evidence qualified experimental arithmetic");
    }
    for(const char* key:{"tile_n","stage_k","window_queries"}){
      auto invalid=tuning;number(field(invalid,key),16.5);
      rejects([&]{(void)amd::tuningPolicy(invalid,requested,1728,960,true);},"fractional specialization accepted");
    }
    for(const char* key:{"fusion","expert_fusion","block_fusion","hardware_publication"}){
      auto invalid=tuning;number(field(invalid,key),0);
      rejects([&]{(void)amd::tuningPolicy(invalid,requested,1728,960,true);},"non-boolean capability accepted");
    }
    auto invalid=tuning;invalid.object.at("geometry").object.at("padded_width").kind=json::Value::String;
    rejects([&]{(void)amd::tuningPolicy(invalid,requested,1728,960,true);},"string geometry accepted");

    auto bound=tuning;
    auto window=json::parse(R"({"key":{"family":"window_attention"},"variant":"amd_window_small",
      "tile_n":64,"stage_k":16,"window_queries":32,"evidence":{"selected":{}}})");
    window.object.at("evidence").object.at("selected")=bound["default_selection"];
    json::Value records;records.kind=json::Value::Array;records.array.push_back(window);
    bound.object["records"]=records;
    auto validate=[&](const json::Value& doc){
      const auto policy=amd::tuningPolicy(doc,requested,1728,960,true);
      amd::validateTuningRecordPolicies(doc,policy);
    };
    validate(bound);
    expect(true,"window's 64-key geometry must not be confused with the N16 GEMM policy");
    auto legacy=bound;legacy.object.at("records").array[0].object.at("evidence").object.erase("selected");
    rejects([&]{validate(legacy);},"old tuning without specialization evidence was accepted");
    for(const char* key:{"tile_n","stage_k","window_queries"}){
      auto edited=bound;number(field(edited,key),std::string(key)=="window_queries" ? 16 : 64);
      rejects([&]{validate(edited);},"edited default inherited another specialization's qualification");
    }
    for(const char* key:{"fusion","expert_fusion","block_fusion","hardware_publication"}){
      auto edited=bound;field(edited,key).boolean=true;
      rejects([&]{validate(edited);},"edited capability inherited another qualification");
    }
    for(const char* key:{"kernels","arithmetic","tile_n","stage_k","window_queries",
                         "fusion","expert_fusion","block_fusion","hardware_publication"}){
      auto missingDefault=bound;missingDefault.object.at("default_selection").object.erase(key);
      rejects([&]{validate(missingDefault);},"missing default policy field received an implicit qualification");
      auto missingProof=bound;missingProof.object.at("records").array[0].object.at("evidence").object.at("selected").object.erase(key);
      rejects([&]{validate(missingProof);},"missing proof policy field received an implicit qualification");
      auto scalar=bound;scalar.object.at("records").array[0].object.at("evidence").object.at("selected").object.at(key).kind=json::Value::Null;
      rejects([&]{validate(scalar);},"nonconforming scalar in qualified policy was accepted");
    }
    for(const char* key:{"tile_n","stage_k","window_queries"}){
      auto edited=bound;number(edited.object.at("records").array[0].object.at(key),32==window[key].number ? 16 : 32);
      rejects([&]{validate(edited);},"edited operator geometry inherited unchanged session proof");
    }
    auto wrongVariant=bound;wrongVariant.object.at("records").array[0].object.at("variant").string="amd_window_optimized";
    rejects([&]{validate(wrongVariant);},"Q64 shader record qualified a Q32 session");
    auto gemm=json::parse(R"({"key":{"family":"fp8_gemm"},"variant":"amd_gemm_optimized",
      "tile_n":16,"stage_k":16,"evidence":{"selected":{}}})");
    gemm.object.at("evidence").object.at("selected")=bound["default_selection"];
    bound.object.at("records").array.push_back(gemm);
    validate(bound);expect(true,"qualified GEMM and window operator policies rejected");
    for(const char* key:{"tile_n","stage_k"}){
      auto edited=bound;number(edited.object.at("records").array[1].object.at(key),32);
      rejects([&]{validate(edited);},"edited GEMM geometry inherited unchanged session proof");
    }
    // Legacy evidence remains readable but cannot qualify only one route.
    for(const auto& [ffn,qkv]:{std::pair{true,false},std::pair{false,true},std::pair{true,true}}){
      auto independent=bound;routes(independent.object.at("default_selection"),ffn,qkv);
      rejects([&]{validate(independent);},"independent default inherited legacy neither-route evidence");
      for(auto& op:independent.object.at("records").array)op.object.at("evidence").object.at("selected")=independent["default_selection"];
      const auto policy=amd::tuningPolicy(independent,requested,1728,960,true);
      amd::validateTuningRecordPolicies(independent,policy);
      expect(policy.ffn32Enabled()==ffn && policy.qkv32Enabled()==qkv && policy.fusion==(ffn&&qkv),"independent qualified selection coupled routes");
      amd::Selection explicitRoute(policy);expect(explicitRoute.forced(),"enabled independent route became an automatic default");
      auto mismatched=policy;mismatched.fusion=false;mismatched.ffn32Fusion=!ffn;
      rejects([&]{(void)amd::tuningPolicy(independent,mismatched,1728,960,false);},"forced independent route was overwritten");
      auto fused=json::parse(R"({"key":{"family":"ffn"},"variant":"amd_ffn32","tile_n":32,"stage_k":32,"evidence":{"selected":{}}})");
      fused.object.at("evidence").object.at("selected")=independent["default_selection"];
      independent.object.at("records").array.push_back(fused);
      if(ffn){validate(independent);expect(true,"enabled independent FFN rejected");}
      else rejects([&]{validate(independent);},"QKV capability qualified disabled FFN");
      independent.object.at("records").array.back().object.at("key").object.at("family").string="qkv_attention";
      independent.object.at("records").array.back().object.at("variant").string="amd_qkv32";
      number(independent.object.at("records").array.back().object.at("tile_n"),64);
      number(independent.object.at("records").array.back().object.at("stage_k"),16);
      if(qkv){validate(independent);expect(true,"enabled independent QKV rejected");}
      else rejects([&]{validate(independent);},"FFN capability qualified disabled QKV");
    }
    auto independent=bound;routes(independent.object.at("default_selection"),true,false);
    for(auto& op:independent.object.at("records").array)op.object.at("evidence").object.at("selected")=independent["default_selection"];
    for(const char* key:{"ffn32_fusion","qkv32_fusion"}){
      auto missing=independent;missing.object.at("default_selection").object.erase(key);
      rejects([&]{validate(missing);},"partial independent default accepted");
      auto badType=independent;number(field(badType,key),1);
      rejects([&]{validate(badType);},"non-Boolean independent default accepted");
      auto missingProof=independent;missingProof.object.at("records").array[0].object.at("evidence").object.at("selected").object.erase(key);
      rejects([&]{validate(missingProof);},"partial independent proof accepted");
      auto editedProof=independent;auto& proof=editedProof.object.at("records").array[0].object.at("evidence").object.at("selected");
      routes(proof,false,true);rejects([&]{validate(editedProof);},"another route's proof qualified independent capability");
    }
    auto conflicting=independent;field(conflicting,"fusion").boolean=true;
    rejects([&]{validate(conflicting);},"conflicting shorthand/effective-route summary accepted");
    auto legacyBoth=record();field(legacyBoth,"fusion").boolean=true;
    const auto legacyPolicy=amd::tuningPolicy(legacyBoth,requested,1728,960,true);
    expect(legacyPolicy.ffn32Enabled() && legacyPolicy.qkv32Enabled(),"legacy shorthand no longer selects both routes");
    for(const char* gemmMode:{"packed","direct"}){
      auto alternative=bound;
      alternative.object.at("default_selection").object["gemm"]=json::parse(std::string("\"")+gemmMode+"\"");
      for(auto& op:alternative.object.at("records").array)op.object.at("evidence").object.at("selected")=alternative["default_selection"];
      const auto policy=amd::tuningPolicy(alternative,requested,1728,960,true);
      alternative.object.at("records").array[1].object.at("variant").string=policy.gemmShaderName();
      validate(alternative);expect(std::string(policy.gemmName())==gemmMode,"qualified GEMM policy was not selected");
      amd::Selection diagnosticGemm(policy);expect(diagnosticGemm.forced(),"non-shared GEMM became automatic");
      rejects([&]{(void)amd::tuningPolicy(tuning,policy,1728,960,false);},"legacy shared tuning replaced a forced GEMM variant");
      auto missingProof=alternative;missingProof.object.at("records").array[0].object.at("evidence").object.at("selected").object.erase("gemm");
      rejects([&]{validate(missingProof);},"non-shared GEMM inherited missing/legacy shared proof");
      auto missingDefault=alternative;missingDefault.object.at("default_selection").object.erase("gemm");
      rejects([&]{validate(missingDefault);},"non-shared proof qualified absent/default shared policy");
      auto otherProof=alternative;otherProof.object.at("records").array[1].object.at("evidence").object.at("selected").object.at("gemm").string=std::string(gemmMode)=="packed"?"direct":"packed";
      rejects([&]{validate(otherProof);},"another GEMM variant's proof qualified this policy");
      auto wrongGemmVariant=alternative;wrongGemmVariant.object.at("records").array[1].object.at("variant").string="amd_gemm_optimized";
      rejects([&]{validate(wrongGemmVariant);},"shared GEMM dispatch record qualified non-shared policy");
      selection.current()=policy;selection.restart();expect(selection.current().gemm==amd::Gemm::Shared,"restart inherited prior tuned GEMM");
      if(std::string(gemmMode)=="direct")for(const auto stage:{32u,64u}){
        auto invalidStage=alternative;number(field(invalidStage,"stage_k"),stage);
        rejects([&]{validate(invalidStage);},"direct tuning claimed nonexistent larger staging");
      }
    }
    for(const char* bad:{"DIRECT","optimized",""}){
      auto invalidGemm=bound;invalidGemm.object.at("default_selection").object["gemm"]=json::parse(std::string("\"")+bad+"\"");
      rejects([&]{validate(invalidGemm);},"invalid GEMM tuning policy accepted");
    }
    auto scalarGemm=bound;number(scalarGemm.object.at("default_selection").object["gemm"],0);
    rejects([&]{validate(scalarGemm);},"non-string GEMM tuning policy accepted");
    if(argc>1){
      std::ifstream input(argv[1],std::ios::binary);if(!input)throw std::runtime_error("cannot read existing audited tuning fixture");
      const auto audited=json::parse(std::string((std::istreambuf_iterator<char>(input)),{}));
      const auto actual=amd::tuningPolicy(audited,requested,1728,960,true);
      amd::validateTuningRecordPolicies(audited,actual);
      const auto expectedGemm=argc>2 ? amd::Options::parseGemm(argv[2]) : amd::Gemm::Shared;
      expect(actual.windowQueries==32 && actual.tileN==16 && actual.stageK==16 && actual.arithmetic==amd::Arithmetic::K16 && actual.gemm==expectedGemm,
             "audited Q32 record policy differs from the expected GEMM selection");
    }
    printf("AMD selection lifetime: %u CPU checks PASS\n",checks);return 0;
  }catch(const std::exception& error){fprintf(stderr,"AMD selection lifetime FAIL: %s\n",error.what());return 1;}
}
