// Tiny read-only JSON parser: enough for the NR/SR manifests and fixtures.
#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Value> array;
  std::map<std::string, Value> object;

  const Value& operator[](const std::string& key) const {
    auto it = object.find(key);
    if (it == object.end()) throw std::runtime_error("missing JSON key " + key);
    return it->second;
  }
  const Value& operator[](size_t index) const {
    if (index >= array.size()) throw std::runtime_error("JSON index out of range");
    return array[index];
  }
  bool has(const std::string& key) const { return object.count(key) != 0; }
  int64_t integer() const { return (int64_t)number; }
  const std::string& str() const { return string; }
  size_t size() const { return kind == Array ? array.size() : object.size(); }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}
  Value parse() {
    Value value = parseValue();
    skipSpace();
    if (position_ != text_.size()) fail("trailing characters");
    return value;
  }

 private:
  [[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("JSON: ") + what + " at " + std::to_string(position_)); }
  void skipSpace() {
    while (position_ < text_.size()) {
      const char c=text_[position_];
      if(c!=' ' && c!='\t' && c!='\n' && c!='\r')break;
      ++position_;
    }
  }
  char peek() { skipSpace(); return position_ < text_.size() ? text_[position_] : '\0'; }
  void expect(char c) { if (peek() != c) fail("unexpected character"); ++position_; }
  void literal(const char* token, size_t length) {
    if(text_.compare(position_,length,token)!=0)fail("invalid literal");
    position_+=length;
  }
  static bool digit(char c) { return c>='0' && c<='9'; }
  double parseNumber() {
    const size_t start=position_;
    if(position_<text_.size() && text_[position_]=='-')++position_;
    if(position_==text_.size())fail("incomplete number");
    if(text_[position_]=='0') {
      ++position_;
      if(position_<text_.size() && digit(text_[position_]))fail("leading zero in number");
    } else {
      if(text_[position_]<'1' || text_[position_]>'9')fail("invalid number");
      while(position_<text_.size() && digit(text_[position_]))++position_;
    }
    if(position_<text_.size() && text_[position_]=='.') {
      ++position_;
      if(position_==text_.size() || !digit(text_[position_]))fail("incomplete number fraction");
      while(position_<text_.size() && digit(text_[position_]))++position_;
    }
    if(position_<text_.size() && (text_[position_]=='e' || text_[position_]=='E')) {
      ++position_;
      if(position_<text_.size() && (text_[position_]=='+' || text_[position_]=='-'))++position_;
      if(position_==text_.size() || !digit(text_[position_]))fail("incomplete number exponent");
      while(position_<text_.size() && digit(text_[position_]))++position_;
    }
    // from_chars is locale independent. Manifest numbers must be representable
    // as finite binary64 values; overflow/underflow beyond that range is rejected.
    double result=0;
    const auto converted=std::from_chars(text_.data()+start,text_.data()+position_,result,std::chars_format::general);
    if(converted.ec!=std::errc() || converted.ptr!=text_.data()+position_ || !std::isfinite(result))
      fail("number outside finite binary64 range");
    return result;
  }
  Value parseValue(uint32_t depth=0) {
    if(depth>512)fail("nesting exceeds 512 levels");
    char c = peek();
    Value value;
    if (c == '{') {
      value.kind = Value::Object;
      ++position_;
      if (peek() == '}') { ++position_; return value; }
      while (true) {
        std::string key = parseString();
        if(value.object.count(key))fail("duplicate object key");
        expect(':');
        value.object.emplace(std::move(key),parseValue(depth+1));
        char next = peek();
        if (next == '}') { ++position_; break; }
        if (next != ',') fail("expected , or }");
        ++position_;
      }
    } else if (c == '[') {
      value.kind = Value::Array;
      ++position_;
      if (peek() == ']') { ++position_; return value; }
      while (true) {
        value.array.push_back(parseValue(depth+1));
        char next = peek();
        if (next == ']') { ++position_; break; }
        if (next != ',') fail("expected , or ]");
        ++position_;
      }
    } else if (c == '"') {
      value.kind = Value::String;
      value.string = parseString();
    } else if (c == 't' || c == 'f') {
      value.kind = Value::Bool;
      value.boolean = c == 't';
      if(value.boolean)literal("true",4);else literal("false",5);
    } else if (c == 'n') {
      literal("null",4);
    } else {
      value.kind = Value::Number;
      value.number = parseNumber();
    }
    return value;
  }
  uint32_t hexCodeUnit() {
    if(text_.size()-position_<4)fail("incomplete Unicode escape");
    uint32_t code=0;
    for(unsigned i=0;i<4;++i) {
      const char c=text_[position_++];
      uint32_t value;
      if(c>='0' && c<='9')value=uint32_t(c-'0');
      else if(c>='a' && c<='f')value=uint32_t(c-'a'+10);
      else if(c>='A' && c<='F')value=uint32_t(c-'A'+10);
      else fail("invalid Unicode escape");
      code=(code<<4)|value;
    }
    return code;
  }
  static void appendCodePoint(std::string& result,uint32_t code) {
    if(code<0x80)result+=(char)code;
    else if(code<0x800) { result+=(char)(0xc0|(code>>6));result+=(char)(0x80|(code&0x3f)); }
    else if(code<0x10000) {
      result+=(char)(0xe0|(code>>12));result+=(char)(0x80|((code>>6)&0x3f));result+=(char)(0x80|(code&0x3f));
    } else {
      result+=(char)(0xf0|(code>>18));result+=(char)(0x80|((code>>12)&0x3f));
      result+=(char)(0x80|((code>>6)&0x3f));result+=(char)(0x80|(code&0x3f));
    }
  }
  void appendUtf8(std::string& result,unsigned char first) {
    unsigned continuation;
    uint32_t code,minimum;
    if(first>=0xc2 && first<=0xdf) { continuation=1;code=first&0x1f;minimum=0x80; }
    else if(first>=0xe0 && first<=0xef) { continuation=2;code=first&0x0f;minimum=0x800; }
    else if(first>=0xf0 && first<=0xf4) { continuation=3;code=first&0x07;minimum=0x10000; }
    else fail("invalid UTF-8 lead byte");
    if(text_.size()-position_<continuation)fail("incomplete UTF-8 sequence");
    const size_t start=position_-1;
    for(unsigned i=0;i<continuation;++i) {
      const auto next=(unsigned char)text_[position_++];
      if((next&0xc0)!=0x80)fail("invalid UTF-8 continuation");
      code=(code<<6)|(next&0x3f);
    }
    if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff))fail("invalid UTF-8 code point");
    result.append(text_,start,continuation+1);
  }
  std::string parseString() {
    expect('"');
    std::string result;
    while (position_ < text_.size()) {
      const auto c=(unsigned char)text_[position_++];
      if (c == '"') return result;
      if (c == '\\') {
        if(position_==text_.size())fail("incomplete string escape");
        char escape = text_[position_++];
        switch (escape) {
          case '"': result+='"';break;
          case '\\': result+='\\';break;
          case '/': result+='/';break;
          case 'n': result += '\n'; break;
          case 't': result += '\t'; break;
          case 'r': result += '\r'; break;
          case 'b': result += '\b'; break;
          case 'f': result += '\f'; break;
          case 'u': {
            uint32_t code=hexCodeUnit();
            if(code>=0xd800 && code<=0xdbff) {
              if(text_.size()-position_<2 || text_[position_]!='\\' || text_[position_+1]!='u')
                fail("missing low Unicode surrogate");
              position_+=2;
              const uint32_t low=hexCodeUnit();
              if(low<0xdc00 || low>0xdfff)fail("invalid low Unicode surrogate");
              code=0x10000+((code-0xd800)<<10)+(low-0xdc00);
            } else if(code>=0xdc00 && code<=0xdfff)fail("unpaired low Unicode surrogate");
            appendCodePoint(result,code);
            break;
          }
          default: fail("invalid string escape");
        }
      } else {
        if(c<0x20)fail("unescaped control character");
        if(c>=0x80)appendUtf8(result,c);else result+=(char)c;
      }
    }
    fail("unterminated string");
  }
  const std::string& text_;
  size_t position_ = 0;
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

}  // namespace json
