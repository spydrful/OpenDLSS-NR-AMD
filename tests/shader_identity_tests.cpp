// CPU-only checks; no Vulkan loader, shader compilation or model is needed.
#include "../src/shader_identity.h"
#include "../src/amd_qualified_fallback.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <cstdio>

namespace {
unsigned checks=0;
void expect(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void rejects(const std::function<void()>& fn,const char* message){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}expect(rejected,message);}
struct Fixture {
  std::filesystem::path root;
  Fixture(){
    root=std::filesystem::temp_directory_path()/std::filesystem::path("opennr-shader-identity-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if(!std::filesystem::create_directory(root))throw std::runtime_error("cannot create fresh shader fixture");
  }
  ~Fixture(){
    // Remove only the two explicitly created files and their now-empty directory.
    std::error_code error;std::filesystem::remove(root/"shader.spv",error);std::filesystem::remove(root/"other.spv",error);std::filesystem::remove(root,error);
  }
  std::string path() const{return (root/"shader.spv").string();}
  void write(const std::vector<uint32_t>& words,size_t bytes=0){
    std::ofstream file(path(),std::ios::binary|std::ios::trunc);
    if(!file.write(reinterpret_cast<const char*>(words.data()),bytes?bytes:words.size()*4))throw std::runtime_error("cannot write shader fixture");
  }
};
}
int main(int argc,char** argv){
  try{
    Fixture fixture;
    std::vector<uint32_t> words{0x07230203,0x00010000,0,1,0,0x00010000};
    fixture.write(words);
    const auto loaded=shader_identity::read(fixture.path());
    expect(loaded.words==words,"loaded shader bytes changed");
    expect(loaded.sha256==shader_identity::digest(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4),"shader identity does not hash its loaded bytes");
    const auto frozen=shader_identity::aggregate({{"module",loaded.sha256}});
    words[2]=1;fixture.write(words);
    const auto replacement=shader_identity::read(fixture.path());
    expect(replacement.sha256!=loaded.sha256,"different disk shader retained the old digest");
    expect(shader_identity::aggregate({{"module",loaded.sha256}})==frozen,"disk replacement changed the cached loaded identity");
    expect(shader_identity::aggregate({{"module",replacement.sha256}})!=frozen,"newly loaded replacement did not receive a new identity");
    std::filesystem::remove(fixture.path());
    expect(shader_identity::aggregate({{"module",loaded.sha256}})==frozen,"disk deletion changed the loaded identity");
    rejects([&]{(void)shader_identity::read(fixture.path());},"missing shader accepted");
    const std::string a(64,'a'),b(64,'b');
    const auto ordered=shader_identity::aggregate({{"alpha",a},{"beta",b}});
    expect(ordered==shader_identity::aggregate({{"beta",b},{"alpha",a}}),"aggregate depends on module insertion order");
    const auto text="alpha:"+a+"\nbeta:"+b+"\n";
    expect(ordered==shader_identity::digest(reinterpret_cast<const uint8_t*>(text.data()),text.size()),"aggregate changed frozen identity encoding");
    expect(ordered!=shader_identity::aggregate({{"alpha",b},{"beta",a}}),"aggregate lost module-name binding");
    rejects([&]{(void)shader_identity::aggregate({});},"empty identity set accepted");
    rejects([&]{(void)shader_identity::aggregate({{"alpha",a},{"alpha",a}});},"duplicate module accepted");
    rejects([&]{(void)shader_identity::aggregate({{"alpha\nbeta",a}});},"module newline accepted");
    rejects([&]{(void)shader_identity::aggregate({{"alpha:beta",a}});},"module delimiter accepted");
    rejects([&]{(void)shader_identity::aggregate({{"",a}});},"empty module name accepted");
    rejects([&]{(void)shader_identity::aggregate({{"alpha",std::string(64,'A')}});},"noncanonical digest accepted");
    rejects([&]{(void)shader_identity::aggregate({{"alpha",std::string(63,'a')}});},"short digest accepted");
    words[0]=0;fixture.write(words);
    rejects([&]{(void)shader_identity::read(fixture.path());},"bad SPIR-V magic accepted");
    words[0]=0x07230203;words[4]=1;fixture.write(words);
    rejects([&]{(void)shader_identity::read(fixture.path());},"bad SPIR-V schema accepted");
    words[4]=0;fixture.write(words,19);
    rejects([&]{(void)shader_identity::read(fixture.path());},"truncated SPIR-V header accepted");
    fixture.write(words,23);
    rejects([&]{(void)shader_identity::read(fixture.path());},"unaligned SPIR-V byte count accepted");
    if(argc>1){
      const auto directory=std::filesystem::path(argv[1]);
      auto aggregate=[&](const char* gemm,const char* window){
        std::vector<std::pair<std::string,std::string>> shaders;
        for(const char* name:{gemm,"portable_f16",window,"amd_global_matrix","ops","preprocess","amd_window_normalize","amd_global_normalize"})
          shaders.emplace_back(name,shader_identity::read((directory/(std::string(name)+".spv")).string()).sha256);
        return shader_identity::aggregate(std::move(shaders));
      };
      expect(aggregate("amd_gemm","amd_window")==amd::qualified::baseline,"frozen baseline aggregate changed");
      expect(aggregate("amd_gemm_optimized","amd_window_optimized")==amd::qualified::shaders,"qualified Q64 aggregate changed");
      expect(aggregate("amd_gemm_optimized","amd_window_small")=="31a0295666cde46ad5d15d190e3ceaced19421531b06dcb7a5282c04999b9348","qualified Q32 aggregate changed");
    }
    printf("Loaded shader identity: %u CPU checks PASS\n",checks);return 0;
  }catch(const std::exception& error){fprintf(stderr,"Loaded shader identity FAIL: %s\n",error.what());return 1;}
}
