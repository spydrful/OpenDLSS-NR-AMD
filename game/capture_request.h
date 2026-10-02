// Bounded diagnostic capture activation; independent of GPU/job ownership.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace nr::capture {
inline uint32_t frameCount(std::string_view value) {
  auto space=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n';};
  while(!value.empty()&&space(value.front()))value.remove_prefix(1);
  while(!value.empty()&&space(value.back()))value.remove_suffix(1);
  if(value.empty())return 1;
  uint32_t result=0;
  for(char c:value){
    if(c<'0'||c>'9'||result>120)throw std::runtime_error("capture.flag must be empty or an integer from 1 through 120");
    result=result*10+uint32_t(c-'0');
  }
  if(result<1||result>120)throw std::runtime_error("capture.flag must be empty or an integer from 1 through 120");
  return result;
}
struct Request {
  bool latched=false;
  uint32_t requested=0,remaining=0;
  void removed(){latched=false;requested=remaining=0;}
  void activate(std::string_view contents){
    if(latched)return;
    // Invalid requests latch too, preventing repeated parse/log work per frame.
    latched=true;requested=remaining=0;requested=remaining=frameCount(contents);
  }
  uint32_t reserve(){
    if(!remaining)throw std::runtime_error("bounded capture request exhausted");
    const uint32_t ordinal=requested-remaining;--remaining;return ordinal;
  }
};
}
