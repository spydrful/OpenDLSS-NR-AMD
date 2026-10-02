// The PE resource / raw weight envelope parser is adapted from
// mochizuki0323/DLSSNR-AMD, commit 82560c4fbfaac347fc5e22c22025191402ae916b.
// Copyright (c) 2026 mochizuki0323. MIT; see tools/MODEL_IMPORTER_NOTICE.txt.
#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nr::importer {
inline constexpr char kSupportedDllSha256[] =
    "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e";
inline constexpr char kSupportedSfDllSha256[] =
    "6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927";
// Published facts only; no code or weights from this independently authored
// reconstruction are copied. Revision pins the original model resource.
inline constexpr char kPinnedResourceProvenance[] =
    "https://huggingface.co/inarikami/dlss5-nr-reverse-engineering/blob/2f3db2562d18f750169c85ed947dc15bffca5a3b/docs/nr-model-access.md";
inline constexpr char kPinnedWeightsResourceSha256[] =
    "836f445d06ecd2e59bb9f17b84b91c143396fd76ccda1c9dc7fe81d5edd548f4";
inline constexpr uint64_t kPinnedWeightsResourceLength = 147695410;
struct Record {
  std::string name;
  uint32_t block = 0, layer = 0;
  std::string parameter;
  std::vector<uint8_t> bytes;
  uint32_t device = 0, typeId = 0, layoutId = 0;
  std::vector<uint32_t> dimensions;
};
struct Stage {
  std::string id, file;
  std::vector<uint8_t> bytes;
};
struct Bundle {
  std::string manifest;
  std::vector<Stage> stages;
};

// Structural parsers are exposed for synthetic malformed-input tests. They do
// not load or execute a DLL and never write files. Import always checks the pin.
std::vector<Record> parseWeightMap(std::span<const uint8_t> bytes);
std::vector<Record> parsePeWeights(std::span<const uint8_t> bytes);
Bundle makeBundle(const std::vector<Record>& records, const std::string& sourceSha256);
// Read-only JSON with hashes/lengths/metadata; never publishes weight bytes.
std::string inspectModel(const std::filesystem::path& dll);
bool isApprovedModelSource(const std::string& dllHash, const std::string& resourceHash, uint64_t resourceLength);
void importModel(const std::filesystem::path& dll, const std::filesystem::path& destination);
}  // namespace nr::importer
