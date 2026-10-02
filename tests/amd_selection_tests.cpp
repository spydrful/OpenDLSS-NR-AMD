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
    if(argc>1){
      std::ifstream input(argv[1],std::ios::binary);if(!input)throw std::runtime_error("cannot read existing audited tuning fixture");
      const auto audited=json::parse(std::string((std::istreambuf_iterator<char>(input)),{}));
      const auto actual=amd::tuningPolicy(audited,requested,1728,960,true);
      amd::validateTuningRecordPolicies(audited,actual);
      expect(actual.windowQueries==32 && actual.tileN==16 && actual.stageK==16 && actual.arithmetic==amd::Arithmetic::K16,
             "existing audited Q32 record policy changed");
    }
    printf("AMD selection lifetime: %u CPU checks PASS\n",checks);return 0;
  }catch(const std::exception& error){fprintf(stderr,"AMD selection lifetime FAIL: %s\n",error.what());return 1;}
}
