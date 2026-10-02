#include "../game/capture_request.h"
#include <cstdio>
#include <stdexcept>
#include <string>
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void fails(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"invalid request accepted");}
}
int main(){try{
  using nr::capture::frameCount;using nr::capture::Request;
  check(frameCount("")==1&&frameCount(" \r\n\t")==1,"empty legacy capture flag changed");
  check(frameCount("1")==1&&frameCount(" 120\n")==120&&frameCount("0002")==2,"valid count rejected");
  for(const auto* text:{"0","121","-1","+2","1 2","2.0","nan","999999999999999999999999","\xEF\xBB\xBF"})fails([&]{frameCount(text);});
  Request request;request.activate("3");check(request.requested==3,"activation count differs");
  check(request.reserve()==0,"first ordinal differs");request.activate("120");check(request.remaining==2,"present flag was reread");
  check(request.reserve()==1&&request.reserve()==2&&request.remaining==0,"bounded reservation differs");fails([&]{request.reserve();});
  request.activate("3");check(request.remaining==0,"exhausted present flag rearmed");request.removed();request.activate("");check(request.reserve()==0&&request.remaining==0,"legacy flag did not rearm");
  request.removed();fails([&]{request.activate("invalid");});check(request.latched&&request.remaining==0,"invalid activation did not latch safely");request.activate("3");check(request.remaining==0,"invalid flag reread without removal");
  request.removed();request.activate("120");for(uint32_t i=0;i<120;++i)check(request.reserve()==i,"120 frame count differs");check(request.remaining==0,"maximum capture request not bounded");
  printf("Capture request tests passed: legacy flag, bounded counts, latching, invalid values and reactivation\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"capture request test failed: %s\n",e.what());return 1;}}
