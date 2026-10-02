#pragma once
// Shader identities describe the exact bytes supplied to vkCreateShaderModule,
// rather than files which may have changed after a session cached its modules.
#include "sha256.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace shader_identity {
inline std::string digest(const uint8_t* bytes, size_t size) {
  auto result = sha256Hex(bytes, size);
  for (char& c : result) if (c >= 'A' && c <= 'F') c += 'a' - 'A';
  return result;
}
struct Source {
  std::vector<uint32_t> words;
  std::string sha256;
};
inline Source read(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) throw std::runtime_error("missing shader " + path);
  const auto length = file.tellg();
  if (length < 20 || length > 64 * 1024 * 1024 || length % 4)
    throw std::runtime_error("invalid SPIR-V byte count " + path);
  Source source;
  source.words.resize(size_t(length) / 4);
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(source.words.data()), length))
    throw std::runtime_error("cannot read complete shader " + path);
  if (source.words[0] != 0x07230203u || source.words[4] != 0)
    throw std::runtime_error("invalid SPIR-V header " + path);
  source.sha256 = digest(reinterpret_cast<const uint8_t*>(source.words.data()), size_t(length));
  return source;
}
inline std::string aggregate(std::vector<std::pair<std::string, std::string>> shaders) {
  std::sort(shaders.begin(), shaders.end());
  std::string identity, previous;
  for (const auto& [name, hash] : shaders) {
    if (name.empty() || name.find_first_of(":\r\n") != std::string::npos || name == previous ||
        hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::runtime_error("invalid loaded shader identity " + name);
    identity += name + ":" + hash + "\n";
    previous = name;
  }
  if (shaders.empty()) throw std::runtime_error("empty loaded shader identity set");
  return digest(reinterpret_cast<const uint8_t*>(identity.data()), identity.size());
}
} // namespace shader_identity
