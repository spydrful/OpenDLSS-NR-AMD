// CPU-only manifest/tuning parser checks. No Vulkan, D3D12, model data or game.
#include "../src/amd_selection.h"
#include <cstdio>
#include <cmath>
#include <fstream>
#include <iterator>
#include <functional>
#include <limits>

namespace {
unsigned checks=0;
void expect(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
void rejects(const std::string& source,const char* message) {
  bool rejected=false;try{(void)json::parse(source);}catch(const std::exception&){rejected=true;}
  expect(rejected,message);
}
std::string read(const char* path) {
  std::ifstream file(path,std::ios::binary);
  if(!file)throw std::runtime_error(std::string("cannot read ")+path);
  return std::string(std::istreambuf_iterator<char>(file),{});
}
void retainsFallback(const std::string& source) {
  amd::Options fallback;amd::Options selected=fallback;bool rejected=false;
  // Same parse-before-selection and catch/restore sequence as loadAmdTuning.
  // No GPU operation is necessary to exercise malformed-input rejection.
  try {
    const auto doc=json::parse(source);
    selected=amd::tuningPolicy(doc,fallback,1728,960,true);
    amd::validateTuningRecordPolicies(doc,selected);
  } catch(const std::exception&) { rejected=true;selected=fallback; }
  expect(rejected,"malformed tuning was accepted");
  expect(selected.windowQueries==64 && selected.tileN==16 && selected.stageK==16 &&
         selected.arithmetic==amd::Arithmetic::K16 && selected.gemm==amd::Gemm::Shared && !selected.ffn32Enabled() && !selected.qkv32Enabled() && !selected.expertFusion &&
         !selected.blockFusion && !selected.hardwarePublication,"malformed tuning changed preserving fallback");
}
}
int main(int argc,char** argv) {
  try {
    const auto literals=json::parse(" [ true, false, null ] \r\n\t");
    expect(literals[0].kind==json::Value::Bool && literals[0].boolean,"true changed");
    expect(literals[1].kind==json::Value::Bool && !literals[1].boolean,"false changed");
    expect(literals[2].kind==json::Value::Null,"null changed");
    for(const char* source:{"t","tr","tru","trux","True","TRUE","f","fals","flase","falsex",
                           "n","nul","nulk","Null","nullx",""," ","[true false]","{\"a\":trux}",
                           "{\"a\":falsx}","{\"a\":nulx}"})rejects(source,"malformed literal accepted");
    for(const char* source:{"{\"a\":1,\"a\":2}","{\"a\":1,\"\\u0061\":2}",
                           "{\"x\":{\"a\":1,\"a\":2}}","[{\"a\":1,\"a\":2}]"})
      rejects(source,"duplicate decoded key accepted");
    expect(json::parse("{\"a\":1,\"b\":{\"a\":2}}")["b"]["a"].number==2,"separate scopes confused");
    for(const char* source:{"{","[","{\"a\"","{\"a\":","{\"a\":1","{\"a\":1,}",
                           "[1","[1,]","[1,,2]","{,}","{\"a\" 1}","[1 2]","{}{}","null[]"})
      rejects(source,"malformed or truncated structure accepted");

    const auto escapes=json::parse(R"("\"\\\/\b\f\n\r\t")").str();
    expect(escapes==std::string("\"\\/\b\f\n\r\t"),"valid short escapes changed");
    expect(json::parse(R"("\u0000")").str()==std::string(1,'\0'),"escaped NUL changed");
    expect(json::parse(R"("\u007f\u0080\u07ff\u0800\uFFFF")").str()==
           std::string("\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf"),"BMP escapes changed");
    expect(json::parse(R"("\uD83D\uDE00")").str()==std::string("\xf0\x9f\x98\x80"),"surrogate pair not decoded");
    expect(json::parse(R"("\uDBFF\uDFFF")").str()==std::string("\xf4\x8f\xbf\xbf"),"maximum Unicode code point changed");
    for(const char* source:{"\"","\"\\","\"\\u","\"\\u0","\"\\u00","\"\\u000",R"("\u00G0")",
                           R"("\u000x")",R"("\q")",R"("\x41")",R"("\v")",R"("\0")",
                           R"("\uD800")",R"("\uD800\u")",R"("\uD800\uD800")",R"("\uD800\u0041")",
                           R"("\uDC00")",R"("\uDFFF")"})rejects(source,"invalid escape accepted");
    for(unsigned c=0;c<32;++c)rejects(std::string("\"a")+char(c)+"b\"","unescaped control accepted");
    for(const auto& bytes:{std::string("\x80"),std::string("\xc0\x80"),std::string("\xc1\xbf"),
                          std::string("\xc2"),std::string("\xc2\x20"),std::string("\xe0\x80\x80"),
                          std::string("\xed\xa0\x80"),std::string("\xed\xbf\xbf"),
                          std::string("\xf0\x80\x80\x80"),std::string("\xf4\x90\x80\x80"),
                          std::string("\xf5\x80\x80\x80"),std::string("\xff")})
      rejects("\""+bytes+"\"","invalid UTF-8 accepted");
    const std::string utf8="\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf";
    expect(json::parse("\""+utf8+"\"").str()==utf8,"valid UTF-8 bytes changed");

    for(const char* source:{"0","-0","123","-123","1.25","0.0","-0.0","1e2","1E-2","1e+2",
                           "1.7976931348623157e308","-1.7976931348623157e308",
                           "2.2250738585072014e-308","4.9406564584124654e-324"}) {
      const auto value=json::parse(source);
      expect(value.kind==json::Value::Number && std::isfinite(value.number),"representable number rejected");
    }
    expect(std::signbit(json::parse("-0").number) && std::signbit(json::parse("-0.0").number),"number signed zero lost");
    expect(json::parse("1.25").number==1.25 && json::parse("1e+2").number==100,"number value changed");
    expect(json::parse("4.9406564584124654e-324").number==std::numeric_limits<double>::denorm_min(),"subnormal number changed");
    for(const char* source:{"+1","01","-01","00",".1","-.1","1.","1.e2","1e","1e+","1e-",
                           "1E+x","0x10","-0x10","NaN","nan","Infinity","Inf","-Inf","1e309",
                           "-1e309","1e-999","[01]","[+1]","[1.]","[1e]","1 2"})
      rejects(source,"invalid/unrepresentable number accepted");
    for(const char* source:{"\vnull","\fnull","null\v","null\f"})rejects(source,"non-JSON whitespace accepted");
    rejects(std::string("null\0",5),"trailing NUL accepted");
    expect(json::parse(std::string(512,'[')+"0"+std::string(512,']')).kind==json::Value::Array,"bounded nesting rejected");
    rejects(std::string(513,'[')+"0"+std::string(513,']'),"unbounded nesting accepted");

    // Read valid tuning/model metadata without executing the GPU runtime or
    // publishing model data; the optional model manifest remains local.
    for(int i=1;i<argc;++i) {
      if(i+1==argc)throw std::runtime_error("expected a path after file option");
      const std::string option=argv[i];const auto source=read(argv[++i]);
      if(option=="--reject")rejects(source,"malformed file accepted");
      else if(option=="--fallback")retainsFallback(source);
      else if(option=="--tuning") {
        const auto doc=json::parse(source);amd::Options requested;
        const auto selected=amd::tuningPolicy(doc,requested,1728,960,true);
        amd::validateTuningRecordPolicies(doc,selected);
        expect(selected.windowQueries==32 && selected.tileN==16 && selected.stageK==16 &&
               selected.arithmetic==amd::Arithmetic::K16 && selected.gemm==amd::Gemm::Shared && !selected.ffn32Enabled() && !selected.qkv32Enabled() && !selected.expertFusion &&
               !selected.blockFusion && !selected.hardwarePublication,"valid shipped tuning changed");
        expect(doc["records"].size()==22,"qualified tuning record count changed");
      } else if(option=="--model") {
        const auto doc=json::parse(source);
        expect(doc["totals"]["blockCount"].number==71 && doc["totals"]["tensorCount"].number==153 &&
               doc["stages"].size()==11,"valid model metadata changed");
      } else if(option=="--file")expect(json::parse(source).kind==json::Value::Object,"valid JSON file rejected");
      else throw std::runtime_error("unknown file option");
    }
    printf("JSON parser: %u CPU checks PASS\n",checks);return 0;
  } catch(const std::exception& error) { fprintf(stderr,"JSON parser FAIL: %s\n",error.what());return 1; }
}
