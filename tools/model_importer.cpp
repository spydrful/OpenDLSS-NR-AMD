// Adapted PE/raw-record parser: Copyright (c) 2026 mochizuki0323 (MIT).
// Original: windows/package/model-tools/dlssnr_extract_model.cpp at
// 82560c4fbfaac347fc5e22c22025191402ae916b. Native stage writer is new.
#include "model_importer.h"
#include "../src/sha256.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "version.lib")
#endif

namespace nr::importer {
namespace {
using View = std::span<const uint8_t>;
[[noreturn]] void bad(const std::string& reason) { throw std::runtime_error(reason); }
View checked(View bytes, uint64_t offset, uint64_t size) {
  if (offset > bytes.size() || size > bytes.size() - offset) bad("out-of-bounds model data");
  return bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
}
uint64_t add(uint64_t a, uint64_t b) {
  if (a > std::numeric_limits<uint64_t>::max() - b) bad("model offset overflow");
  return a + b;
}
uint64_t le(View bytes, uint64_t offset, unsigned count) {
  auto value = checked(bytes, offset, count);
  uint64_t result = 0;
  for (unsigned i = 0; i < count; ++i) result |= uint64_t(value[i]) << (8 * i);
  return result;
}
uint16_t rd16(View b, uint64_t o) { return static_cast<uint16_t>(le(b, o, 2)); }
uint32_t rd32(View b, uint64_t o) { return static_cast<uint32_t>(le(b, o, 4)); }
uint64_t rd64(View b, uint64_t o) { return le(b, o, 8); }
std::string nativeHash(View bytes) {
  // The original hash helper does pointer arithmetic, even for an empty input.
  static const uint8_t empty = 0;
  return sha256Hex(bytes.empty() ? &empty : bytes.data(), bytes.size());
}
std::string lowerHash(View bytes) {
  auto hash = nativeHash(bytes);
  for (char& c : hash) if (c >= 'A' && c <= 'F') c += 'a' - 'A';
  return hash;
}
std::string fileVersion(const std::filesystem::path& path) {
#ifdef _WIN32
  DWORD unused = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
  if (!size || size > (1u << 20)) return "unavailable";
  std::vector<uint8_t> bytes(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return "unavailable";
  void* data = nullptr; UINT length = 0;
  if (!VerQueryValueW(bytes.data(), L"\\", &data, &length) || length < sizeof(VS_FIXEDFILEINFO)) return "unavailable";
  const auto& info = *static_cast<const VS_FIXEDFILEINFO*>(data);
  if (info.dwSignature != VS_FFI_SIGNATURE) return "unavailable";
  return std::to_string(info.dwFileVersionMS >> 16) + "." + std::to_string(info.dwFileVersionMS & 65535) + "." +
         std::to_string(info.dwFileVersionLS >> 16) + "." + std::to_string(info.dwFileVersionLS & 65535);
#else
  (void)path;
  return "unavailable";
#endif
}
uint32_t number(const std::string& value, const char* label) {
  uint32_t result = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (value.empty() || error != std::errc() || end != value.data() + value.size())
    bad(std::string("invalid ") + label + " in tensor name");
  return result;
}
Record namedRecord(const std::string& name, View payload) {
  if (!name.starts_with("block")) bad("unexpected tensor name: " + name);
  const size_t layer = name.find(".layer", 5);
  const size_t parameter = layer == std::string::npos ? layer : name.find('.', layer + 6);
  if (layer == std::string::npos || parameter == std::string::npos) bad("invalid tensor name: " + name);
  Record r;
  r.name = name;
  r.block = number(name.substr(5, layer - 5), "block");
  r.layer = number(name.substr(layer + 6, parameter - layer - 6), "layer");
  r.parameter = name.substr(parameter + 1);
  if (r.block >= 71 || r.layer >= 128 || r.parameter.empty()) bad("tensor name is outside the supported model");
  for (char c : r.parameter)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      bad("unsafe tensor parameter name");
  if (name != "block" + std::to_string(r.block) + ".layer" + std::to_string(r.layer) + "." + r.parameter)
    bad("noncanonical tensor name");
  r.bytes.assign(payload.begin(), payload.end());
  return r;
}
struct Section { uint32_t rva, offset, size; };
class Pe {
  View bytes_;
  std::vector<Section> sections_;
  uint32_t resourceRva_ = 0, resourceSize_ = 0;
  uint64_t offset(uint64_t rva, uint64_t size) const {
    for (const auto& s : sections_) {
      if (rva >= s.rva && rva - s.rva <= s.size && size <= s.size - (rva - s.rva))
        return add(s.offset, rva - s.rva);
    }
    bad("resource RVA is not backed by the PE file");
  }
  void walk(View tree, uint64_t relative, unsigned depth, bool weights,
            std::set<uint64_t>& active, size_t& visited, std::vector<View>& matches) const {
    if (depth > 8 || !active.insert(relative).second) bad("cyclic or excessively deep resource tree");
    if (++visited > 65536) bad("resource tree has too many nodes");
    const uint32_t count = uint32_t(rd16(tree, add(relative, 12))) + rd16(tree, add(relative, 14));
    checked(tree, add(relative, 16), uint64_t(count) * 8);
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t entry = add(add(relative, 16), uint64_t(i) * 8);
      const uint32_t name = rd32(tree, entry), child = rd32(tree, entry + 4);
      bool found = false;
      if (name & 0x80000000u) {
        const uint64_t at = name & 0x7fffffffu;
        const uint16_t length = rd16(tree, at);
        auto label = checked(tree, at + 2, uint64_t(length) * 2);
        for (uint32_t j = 0; j < length; ++j) {
          const uint16_t c = rd16(label, j * 2);
          if (c >= 0xd800 && c < 0xdc00) {
            if (j + 1 >= length) bad("invalid UTF-16 resource name");
            const uint16_t next = rd16(label, (++j) * 2);
            if (next < 0xdc00 || next >= 0xe000) bad("invalid UTF-16 resource name");
          } else if (c >= 0xdc00 && c < 0xe000) bad("invalid UTF-16 resource name");
        }
        constexpr char wanted[] = "WEIGHTS_HT";
        found = length == 10;
        for (unsigned j = 0; found && j < 10; ++j) found = rd16(label, j * 2) == wanted[j];
      }
      if (child & 0x80000000u) {
        walk(tree, child & 0x7fffffffu, depth + 1, weights || found, active, visited, matches);
      } else {
        checked(tree, child, 16);
        const uint32_t rva = rd32(tree, child), size = rd32(tree, uint64_t(child) + 4);
        if (weights || found) {
          matches.push_back(checked(bytes_, offset(rva, size), size));
          if (matches.size() > 1) bad("expected exactly one WEIGHTS_HT resource");
        }
      }
    }
    active.erase(relative);
  }
 public:
  explicit Pe(View bytes) : bytes_(bytes) {
    auto mz = checked(bytes, 0, 2);
    if (mz[0] != 'M' || mz[1] != 'Z') bad("not a PE file");
    const uint64_t pe = rd32(bytes, 0x3c);
    if (std::memcmp(checked(bytes, pe, 4).data(), "PE\0\0", 4)) bad("missing PE signature");
    if (rd16(bytes, pe + 4) != 0x8664) bad("expected an x64 PE file");
    const uint16_t count = rd16(bytes, pe + 6), optionalSize = rd16(bytes, pe + 20);
    const uint64_t opt = pe + 24;
    checked(bytes, opt, optionalSize);
    if (optionalSize < 144 || rd16(bytes, opt) != 0x20b) bad("expected PE32+ optional header");
    if (rd32(bytes, opt + 108) < 3) bad("PE resource directory is missing");
    resourceRva_ = rd32(bytes, opt + 128);
    resourceSize_ = rd32(bytes, opt + 132);
    if (!count || count > 96) bad("invalid PE section count");
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t at = opt + optionalSize + uint64_t(i) * 40;
      checked(bytes, at, 40);
      Section s { rd32(bytes, at + 12), rd32(bytes, at + 20), rd32(bytes, at + 16) };
      checked(bytes, s.offset, s.size);
      sections_.push_back(s);
    }
  }
  View weights() const {
    if (!resourceRva_ || !resourceSize_) bad("PE has no resource tree");
    View tree = checked(bytes_, offset(resourceRva_, resourceSize_), resourceSize_);
    std::set<uint64_t> active;
    size_t visited = 0;
    std::vector<View> matches;
    walk(tree, 0, 0, false, active, visited, matches);
    if (matches.size() != 1) bad("expected exactly one WEIGHTS_HT resource");
    return matches.front();
  }
};
std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  if (!std::filesystem::is_regular_file(path)) bad("input is not a regular file");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) bad("cannot open the source DLL");
  auto size = file.tellg();
  if (size < 0 || size > std::streamoff(1ull << 30)) bad("source DLL must be at most 1 GiB");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) bad("cannot read the source DLL");
  return bytes;
}
void writeFile(const std::filesystem::path& path, View bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    bad("cannot write the imported model");
  file.close();
  if (!file) bad("cannot finish writing the imported model");
}
}  // namespace

std::vector<Record> parseWeightMap(View bytes) {
  const uint64_t total = rd64(bytes, 0);
  if (total != bytes.size()) bad("weight map size mismatch");
  uint64_t at = 8;
  std::set<std::string> names;
  std::vector<Record> records;
  while (at < total) {
    const uint64_t nameSize = rd64(bytes, at);
    at = add(at, 8);
    if (!nameSize || nameSize > 4096) bad("invalid weight name length");
    const auto nameBytes = checked(bytes, at, nameSize);
    const std::string name(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
    if (!names.insert(name).second) bad("duplicate weight name");
    if (records.size() >= 4096) bad("too many weight records");
    at = add(at, nameSize);
    const uint64_t outer = rd64(bytes, at);
    at = add(at, 8);
    const auto body = checked(bytes, at, outer);
    const uint64_t inner = rd64(body, 0), raw = rd64(body, 8);
    if (inner != outer) bad("inner and outer weight sizes differ");
    if (!raw || raw > std::numeric_limits<uint32_t>::max() || rd32(body, 16) > 1)
      bad("empty weight payload or unsupported device");
    const auto payload = checked(body, 20, raw);
    const uint32_t typeId = rd32(body, add(20, raw)), layoutId = rd32(body, add(24, raw));
    const uint64_t dimensions = rd64(body, add(28, raw));
    if (dimensions > 32) bad("implausible weight dimension count");
    checked(body, add(36, raw), dimensions * 4);
    if (add(add(36, raw), dimensions * 4) != inner) bad("trailing or missing weight metadata");
    auto record = namedRecord(name, payload);
    record.device = rd32(body, 16); record.typeId = typeId; record.layoutId = layoutId;
    for (uint64_t i = 0; i < dimensions; ++i) record.dimensions.push_back(rd32(body, add(add(36, raw), i * 4)));
    records.push_back(std::move(record));
    at = add(at, outer);
  }
  if (records.empty()) bad("weight map has no tensors");
  return records;
}
std::vector<Record> parsePeWeights(View bytes) { return parseWeightMap(Pe(bytes).weights()); }
bool isApprovedModelSource(const std::string& dllHash, const std::string& resourceHash, uint64_t resourceLength) {
  return (dllHash == kSupportedDllSha256 || dllHash == kSupportedSfDllSha256) &&
         resourceHash == kPinnedWeightsResourceSha256 && resourceLength == kPinnedWeightsResourceLength;
}

std::string inspectModel(const std::filesystem::path& source) {
  const auto bytes = readFile(source);
  const auto resource = Pe(bytes).weights();
  const auto records = parseWeightMap(resource);
  const auto sourceHash = lowerHash(bytes), resourceHash = lowerHash(resource);
  const auto bundle = makeBundle(records, sourceHash);
  std::set<uint32_t> blocks;
  uint64_t payloadBytes = 0;
  for (const auto& record : records) { blocks.insert(record.block); payloadBytes += record.bytes.size(); }
  std::ostringstream report;
  report << "{\n  \"format\": \"OpenNR-model-inspection-v1\",\n  \"fileVersionNumber\": \"" << fileVersion(source)
         << "\",\n  \"sourceByteLength\": " << bytes.size() << ",\n  \"sourceSha256\": \"" << sourceHash
         << "\",\n  \"acceptedByStrictDllPin\": " << ((sourceHash == kSupportedDllSha256 || sourceHash == kSupportedSfDllSha256) ? "true" : "false")
         << ",\n  \"weightsResource\": {\"name\": \"WEIGHTS_HT\", \"fileOffset\": " << (resource.data() - bytes.data())
         << ", \"byteLength\": " << resource.size() << ", \"sha256\": \"" << resourceHash
         << "\", \"matchesPublishedPinnedResource\": "
         << (resource.size() == kPinnedWeightsResourceLength && resourceHash == kPinnedWeightsResourceSha256 ? "true" : "false")
         << "},\n  \"publishedResourceProvenance\": \"" << kPinnedResourceProvenance << "\",\n"
         << "  \"counts\": {\"records\": " << records.size() << ", \"blocks\": " << blocks.size()
         << ", \"payloadBytes\": " << payloadBytes << ", \"expected153Records71Blocks\": "
         << (records.size() == 153 && blocks.size() == 71 ? "true" : "false") << "},\n  \"rawTensorMetadata\": [\n";
  for (size_t i = 0; i < records.size(); ++i) {
    const auto& r = records[i];
    report << "    {\"name\": \"" << r.name << "\", \"byteLength\": " << r.bytes.size() << ", \"sha256\": \""
           << lowerHash(r.bytes) << "\", \"device\": " << r.device << ", \"typeId\": " << r.typeId
           << ", \"layoutId\": " << r.layoutId << ", \"dimensions\": [";
    for (size_t d = 0; d < r.dimensions.size(); ++d) report << (d ? ", " : "") << r.dimensions[d];
    report << "]}" << (i + 1 == records.size() ? "\n" : ",\n");
  }
  report << "  ],\n  \"nativeLayout\": " << bundle.manifest << "}\n";
  return report.str();
}

Bundle makeBundle(const std::vector<Record>& records, const std::string& sourceHash) {
  if (sourceHash.size() != 64 || !std::all_of(sourceHash.begin(), sourceHash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) bad("source digest must be 64 lowercase hexadecimal characters");
  constexpr std::array<uint32_t, 11> ends = {4, 8, 14, 22, 30, 38, 47, 55, 61, 65, 70};
  Bundle out;
  for (unsigned i = 0; i < ends.size(); ++i) {
    const std::string id = "stage" + std::to_string(i);
    out.stages.push_back({id, id + ".bin", {}});
  }
  std::vector<const Record*> ordered;
  for (const auto& record : records) ordered.push_back(&record);
  std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
    if (a->block != b->block) return a->block < b->block;
    if (a->layer != b->layer) return a->layer < b->layer;
    return a->parameter < b->parameter;
  });
  struct Slice { const Record* record; size_t stage, offset; };
  std::vector<Slice> slices;
  std::set<std::string> names;
  for (const auto* r : ordered) {
    // Revalidate callers of this public, in-memory helper as well as parsed input.
    const auto checkedRecord = namedRecord(r->name, r->bytes);
    if (checkedRecord.block != r->block || checkedRecord.layer != r->layer || checkedRecord.parameter != r->parameter ||
        r->bytes.empty() || !names.insert(r->name).second) bad("inconsistent or duplicate tensor record");
    size_t index = std::lower_bound(ends.begin(), ends.end(), r->block) - ends.begin();
    auto& stage = out.stages[index];
    if (r->bytes.size() > std::numeric_limits<uint32_t>::max() ||
        stage.bytes.size() > std::numeric_limits<uint32_t>::max() - r->bytes.size()) bad("stage is too large");
    slices.push_back({r, index, stage.bytes.size()});
    stage.bytes.insert(stage.bytes.end(), r->bytes.begin(), r->bytes.end());
  }
  std::ostringstream manifest;
  manifest << "{\n  \"format\": \"OpenDLSS-NR-native-stages-v1\",\n  \"sourceSha256\": \"" << sourceHash
           << "\",\n  \"totals\": {\"blockCount\": 71, \"tensorCount\": " << slices.size() << "},\n  \"stages\": [\n";
  for (size_t i = 0; i < out.stages.size(); ++i) {
    const auto& stage = out.stages[i];
    manifest << "    {\"id\": \"" << stage.id << "\", \"file\": \"" << stage.file << "\", \"packedByteLength\": "
             << stage.bytes.size() << ", \"sha256\": \"" << nativeHash(stage.bytes) << "\"}"
             << (i + 1 == out.stages.size() ? "\n" : ",\n");
  }
  manifest << "  ],\n  \"tensors\": [\n";
  for (size_t i = 0; i < slices.size(); ++i) {
    const auto& slice = slices[i]; const auto& r = *slice.record;
    manifest << "    {\"name\": \"" << r.name << "\", \"block\": " << r.block << ", \"layer\": " << r.layer
             << ", \"parameter\": \"" << r.parameter << "\", \"stage\": \"" << out.stages[slice.stage].id
             << "\", \"stageOffset\": " << slice.offset << ", \"byteLength\": " << r.bytes.size() << "}"
             << (i + 1 == slices.size() ? "\n" : ",\n");
  }
  manifest << "  ]\n}\n";
  out.manifest = manifest.str();
  return out;
}

void importModel(const std::filesystem::path& source, const std::filesystem::path& destination) {
  const auto absolute = std::filesystem::absolute(destination).lexically_normal();
  if (std::filesystem::exists(absolute)) bad("destination already exists; import into a new directory");
  const auto bytes = readFile(source);
  const std::string digest = lowerHash(bytes);
  if (digest != kSupportedDllSha256 && digest != kSupportedSfDllSha256) bad("unsupported source DLL SHA-256 " + digest + "; an explicitly pinned container is required");
  const auto resource = Pe(bytes).weights();
  if (!isApprovedModelSource(digest, lowerHash(resource), resource.size())) bad("source DLL's model resource does not match the pinned length and SHA-256");
  const auto records = parseWeightMap(resource);
  if (records.size() != 153) bad("supported DLL must contain exactly 153 raw tensor records");
  std::set<uint32_t> blocks;
  for (const auto& record : records) blocks.insert(record.block);
  if (blocks.size() != 71) bad("supported DLL must contain all 71 model blocks");
  const auto bundle = makeBundle(records, digest);
  std::filesystem::create_directories(absolute.parent_path());
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto temporary = absolute.parent_path() / (absolute.filename().native() + std::filesystem::path(".import-" + std::to_string(stamp)).native());
  if (!std::filesystem::create_directory(temporary)) bad("cannot reserve temporary model directory");
  try {
    std::filesystem::create_directory(temporary / "model");
    for (const auto& stage : bundle.stages) writeFile(temporary / "model" / stage.file, stage.bytes);
    writeFile(temporary / "manifest.json", View(reinterpret_cast<const uint8_t*>(bundle.manifest.data()), bundle.manifest.size()));
    // No REPLACE_EXISTING: another importer or user-created destination wins.
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), absolute.c_str(), 0)) bad("cannot publish imported model; destination may already exist");
#else
    if (std::filesystem::exists(absolute)) bad("destination appeared while importing");
    std::filesystem::rename(temporary, absolute);
#endif
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);
    throw;
  }
}
}  // namespace nr::importer

#ifndef NR_IMPORTER_NO_MAIN
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  try {
    if (argc == 3 && std::filesystem::path(argv[1]) == std::filesystem::path("--inspect")) {
      std::cout << nr::importer::inspectModel(argv[2]);
      return 0;
    }
    if (argc != 3) {
      std::cerr << "Usage: model_importer <pinned-nvngx_dlssnr.dll> <new-model-directory>\n";
      std::cerr << "       model_importer --inspect <local-DLL> (metadata/hashes only; read-only)\n";
      return 2;
    }
    nr::importer::importModel(argv[1], argv[2]);
    std::cout << "Imported 71 blocks / 153 raw tensors into 11 native stage files.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Model import failed: " << error.what() << '\n';
    return 1;
  }
}
#endif
