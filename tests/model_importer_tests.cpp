#include "../tools/model_importer.h"
#include "../src/json.h"
#include "../src/sha256.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using Bytes = std::vector<uint8_t>;
static int checks = 0;
void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
void rejects(const std::function<void()>& fn, const char* reason) {
  ++checks;
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error(reason);
}
void put(Bytes& bytes, size_t offset, uint64_t value, unsigned width) {
  if (bytes.size() < offset + width) bytes.resize(offset + width);
  for (unsigned i = 0; i < width; ++i) bytes[offset + i] = uint8_t(value >> (i * 8));
}
void append(Bytes& bytes, uint64_t value, unsigned width) { put(bytes, bytes.size(), value, width); }
void appendRecord(Bytes& bytes, const std::string& name, const Bytes& payload = {0x00, 0x7f, 0xff, 0x42}) {
  append(bytes, name.size(), 8);
  bytes.insert(bytes.end(), name.begin(), name.end());
  const size_t bodySize = 36 + payload.size() + 4;
  append(bytes, bodySize, 8);
  append(bytes, bodySize, 8);
  append(bytes, payload.size(), 8);
  append(bytes, 0, 4);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  append(bytes, 1, 4); append(bytes, 2, 4); append(bytes, 1, 8); append(bytes, payload.size(), 4);
  put(bytes, 0, bytes.size(), 8);
}
Bytes weightMap() {
  Bytes bytes(8);
  appendRecord(bytes, "block0.layer0.layer");
  appendRecord(bytes, "block70.layer0.layer", {1, 2});
  return bytes;
}
Bytes pe(const Bytes& weights) {
  // One .rsrc section; named root -> numeric resource -> language -> data.
  constexpr size_t file = 0x200, treeSize = 0x100, rawSize = 0x1000;
  Bytes bytes(file + rawSize);
  bytes[0] = 'M'; bytes[1] = 'Z'; put(bytes, 0x3c, 0x80, 4);
  bytes[0x80] = 'P'; bytes[0x81] = 'E'; put(bytes, 0x84, 0x8664, 2); put(bytes, 0x86, 1, 2);
  put(bytes, 0x94, 0xf0, 2); put(bytes, 0x98, 0x20b, 2); put(bytes, 0x98 + 108, 16, 4);
  put(bytes, 0x98 + 128, 0x1000, 4); put(bytes, 0x98 + 132, treeSize, 4);
  const size_t section = 0x98 + 0xf0;
  put(bytes, section + 12, 0x1000, 4); put(bytes, section + 16, rawSize, 4); put(bytes, section + 20, file, 4);
  put(bytes, file + 12, 1, 2); put(bytes, file + 16, 0x80000080u, 4); put(bytes, file + 20, 0x80000020u, 4);
  put(bytes, file + 0x20 + 14, 1, 2); put(bytes, file + 0x30, 1, 4); put(bytes, file + 0x34, 0x80000040u, 4);
  put(bytes, file + 0x40 + 14, 1, 2); put(bytes, file + 0x50, 1033, 4); put(bytes, file + 0x54, 0x60, 4);
  put(bytes, file + 0x60, 0x1100, 4); put(bytes, file + 0x64, weights.size(), 4);
  put(bytes, file + 0x80, 10, 2);
  const std::string label = "WEIGHTS_HT";
  for (size_t i = 0; i < label.size(); ++i) put(bytes, file + 0x82 + 2 * i, label[i], 2);
  std::copy(weights.begin(), weights.end(), bytes.begin() + file + treeSize);
  return bytes;
}
int main() {
  try {
    require(nr::importer::isApprovedModelSource(nr::importer::kSupportedDllSha256, nr::importer::kPinnedWeightsResourceSha256, nr::importer::kPinnedWeightsResourceLength), "original pinned container rejected");
    require(nr::importer::isApprovedModelSource(nr::importer::kSupportedSfDllSha256, nr::importer::kPinnedWeightsResourceSha256, nr::importer::kPinnedWeightsResourceLength), "verified alternate container rejected");
    require(!nr::importer::isApprovedModelSource(std::string(64, '0'), nr::importer::kPinnedWeightsResourceSha256, nr::importer::kPinnedWeightsResourceLength), "unapproved container accepted by resource pin alone");
    require(!nr::importer::isApprovedModelSource(nr::importer::kSupportedSfDllSha256, std::string(64, '0'), nr::importer::kPinnedWeightsResourceLength), "alternate container accepted with wrong resource hash");
    require(!nr::importer::isApprovedModelSource(nr::importer::kSupportedDllSha256, nr::importer::kPinnedWeightsResourceSha256, nr::importer::kPinnedWeightsResourceLength - 1), "original container accepted with wrong resource length");
    auto raw = weightMap();
    const auto records = nr::importer::parseWeightMap(raw);
    require(records.size() == 2 && records[0].bytes == Bytes({0x00, 0x7f, 0xff, 0x42}), "raw tensor bytes changed");
    const auto fromPe = nr::importer::parsePeWeights(pe(raw));
    require(fromPe.size() == 2 && fromPe[1].block == 70, "valid PE resource was not parsed");
    auto bundle = nr::importer::makeBundle(records, nr::importer::kSupportedDllSha256);
    require(bundle.stages.size() == 11 && bundle.stages[0].bytes == records[0].bytes && bundle.stages[10].bytes == records[1].bytes,
            "native stage boundaries or raw payload changed");
    auto manifest = json::parse(bundle.manifest);
    require(manifest["totals"]["blockCount"].integer() == 71 && manifest["tensors"].array.size() == 2, "manifest contract mismatch");
    require(manifest["stages"].array[0]["sha256"].str() == sha256Hex(records[0].bytes.data(), records[0].bytes.size()), "stage hash case does not match Model");
    require(manifest["stages"].array[1]["sha256"].str() == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855",
            "empty stage hash is invalid");
    rejects([&] { nr::importer::makeBundle(records, "\"injected\""); }, "unsafe source digest accepted");
    Bytes allBlocks(8);
    for (unsigned block = 0; block < 71; ++block)
      appendRecord(allBlocks, "block" + std::to_string(block) + ".layer0.layer", {uint8_t(block)});
    const auto allBundle = nr::importer::makeBundle(nr::importer::parseWeightMap(allBlocks), nr::importer::kSupportedDllSha256);
    const std::vector<Bytes> expected = {{0,1,2,3,4},{5,6,7,8},{9,10,11,12,13,14},{15,16,17,18,19,20,21,22},
      {23,24,25,26,27,28,29,30},{31,32,33,34,35,36,37,38},{39,40,41,42,43,44,45,46,47},
      {48,49,50,51,52,53,54,55},{56,57,58,59,60,61},{62,63,64,65},{66,67,68,69,70}};
    for (size_t stage = 0; stage < expected.size(); ++stage)
      require(allBundle.stages[stage].bytes == expected[stage], "model block was assigned to the wrong native stage");
    for (size_t length = 0; length < raw.size(); ++length) {
      auto truncated = Bytes(raw.begin(), raw.begin() + length);
      rejects([&] { nr::importer::parseWeightMap(truncated); }, "truncated weight map accepted");
    }
    Bytes duplicate(8); appendRecord(duplicate, "block0.layer0.layer"); appendRecord(duplicate, "block0.layer0.layer");
    rejects([&] { nr::importer::parseWeightMap(duplicate); }, "duplicate tensor accepted");
    Bytes unsafe(8); appendRecord(unsafe, "block0.layer0.../escape");
    rejects([&] { nr::importer::parseWeightMap(unsafe); }, "unsafe tensor name accepted");
    Bytes outOfRange(8); appendRecord(outOfRange, "block71.layer0.layer");
    rejects([&] { nr::importer::parseWeightMap(outOfRange); }, "out-of-range block accepted");
    Bytes empty(8); appendRecord(empty, "block0.layer0.layer", {});
    rejects([&] { nr::importer::parseWeightMap(empty); }, "empty tensor accepted");
    auto mutated = raw;
    const size_t body = 8 + 8 + std::string("block0.layer0.layer").size() + 8;
    put(mutated, body, 99, 8);
    rejects([&] { nr::importer::parseWeightMap(mutated); }, "mismatched envelope accepted");
    mutated = raw; put(mutated, body + 16, 2, 4);
    rejects([&] { nr::importer::parseWeightMap(mutated); }, "non-host device accepted");
    mutated = raw; put(mutated, body + 28 + 4, 33, 8);
    rejects([&] { nr::importer::parseWeightMap(mutated); }, "excessive dimension count accepted");
    auto executable = pe(raw);
    put(executable, 0x200 + 20, 0x80000000u, 4);
    rejects([&] { nr::importer::parsePeWeights(executable); }, "cyclic resource tree accepted");
    executable = pe(raw); put(executable, 0x200 + 0x64, 0xffffffffu, 4);
    rejects([&] { nr::importer::parsePeWeights(executable); }, "out-of-bounds resource accepted");
    executable = pe(raw); put(executable, 0x200 + 0x82, 0xd800, 2);
    rejects([&] { nr::importer::parsePeWeights(executable); }, "invalid UTF-16 resource name accepted");
    executable = pe(raw); put(executable, 0x98 + 128, 0, 4);
    rejects([&] { nr::importer::parsePeWeights(executable); }, "missing resource accepted");
    executable = pe(raw); put(executable, 0x84, 0x14c, 2);
    rejects([&] { nr::importer::parsePeWeights(executable); }, "non-x64 PE accepted");
    for (size_t length : {size_t(0), size_t(1), size_t(0x40), size_t(0x100), size_t(0x200)}) {
      auto truncated = Bytes(executable.begin(), executable.begin() + length);
      rejects([&] { nr::importer::parsePeWeights(truncated); }, "truncated PE accepted");
    }
    const auto dir = std::filesystem::temp_directory_path() / "open-nr-importer-synthetic-test";
    if (std::filesystem::exists(dir)) throw std::runtime_error("test scratch directory already exists");
    std::filesystem::create_directory(dir);
    try {
      const auto dll = dir / "synthetic.dll", destination = dir / "result";
      std::ofstream file(dll, std::ios::binary); file.write(reinterpret_cast<const char*>(executable.data()), executable.size()); file.close();
      const auto valid = pe(raw);
      std::ofstream inspected(dll, std::ios::binary); inspected.write(reinterpret_cast<const char*>(valid.data()), valid.size()); inspected.close();
      const auto report = json::parse(nr::importer::inspectModel(dll));
      require(report["counts"]["records"].integer() == 2 && uint64_t(report["weightsResource"]["byteLength"].integer()) == raw.size(), "inspection metadata mismatch");
      require(report["weightsResource"]["sha256"].str() == [&] { auto s = sha256Hex(raw.data(), raw.size()); for (char& c:s) if(c>='A'&&c<='F')c+='a'-'A';return s; }(), "inspection resource hash mismatch");
      require(report["rawTensorMetadata"].array[0]["dimensions"].array[0].integer() == 4, "inspection dimensions mismatch");
      require(!std::filesystem::exists(destination), "read-only inspection created output");
      rejects([&] { nr::importer::importModel(dll, destination); }, "unpinned DLL accepted");
      require(!std::filesystem::exists(destination), "failed import published a destination");
      std::filesystem::create_directory(destination);
      std::ofstream(destination / "keep.txt") << "keep";
      rejects([&] { nr::importer::importModel(dll, destination); }, "existing destination accepted");
      require(std::filesystem::is_regular_file(destination / "keep.txt"), "existing destination changed");
      std::filesystem::remove_all(dir);
    } catch (...) { std::filesystem::remove_all(dir); throw; }
    std::cout << "Model importer: " << checks << " checks passed (synthetic inputs; real DLL not supplied).\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
