// Standalone CPU-only contract tests: no Vulkan instance, loader or GPU needed.
#include "amd_fusion_validation.h"
#include <cstdio>
#include <functional>

namespace {
uint64_t nextHandle = 1;
vk::Buffer fakeBuffer(uint64_t bytes) {
  vk::Buffer result;
  result.buffer = (VkBuffer)(uintptr_t)nextHandle++;
  result.size = bytes;
  return result;
}
nr::Activation fakeActivation(uint32_t rows, uint32_t channels, nr::Format format) {
  nr::Activation a;
  a.rows = rows; a.allocRows = nr::alignRows(rows); a.channels = channels; a.format = format;
  a.buffer = fakeBuffer(uint64_t(a.allocRows) * channels * nr::formatBytes(format));
  return a;
}
struct Fixture {
  nr::Activation input, residual, raw, output;
  vk::Buffer expand, contract, project, qkv, prior;
  nr::Tensor aux;
  explicit Fixture(uint32_t channels = 32)
      : input(fakeActivation(77, channels, nr::Format::E4)),
        residual(fakeActivation(77, channels, nr::Format::F16)),
        raw(fakeActivation(77, channels, nr::Format::F16)),
        output(fakeActivation(77, channels, nr::Format::E4)),
        expand(fakeBuffer(uint64_t(channels / 32) * channels * 128)),
        contract(fakeBuffer(uint64_t(channels) * 128)),
        project(fakeBuffer(uint64_t(channels) * channels)),
        qkv(fakeBuffer(32 * 96)), prior(fakeBuffer(64 * 64 * 2)) {
    aux.raw = fakeBuffer(512); aux.byteLength = 512;
  }
  nr::AmdFfn32Args ffn() {
    return {&input, &residual, &expand, &contract, &aux, &raw, &output, 77, 0};
  }
  nr::AmdQkv32Args attention() {
    return {&input, &qkv, &prior, &aux, &output, 128, 11, 7, 4, 4};
  }
  nr::AmdExpertFfnArgs expert() {
    return {&input, &residual, &expand, &contract, &project, &aux, &raw, &output,
            77, input.channels, 0};
  }
  nr::AmdBlock32Args block() {
    return {&input, &residual, &expand, &contract, &qkv, &project, &prior, &aux,
            &output, &raw, 0, 64, 128, 11, 7, 4, 4};
  }
};
int checks = 0;
void test(const char* name, bool expectReject, const std::function<void()>& body) {
  bool rejected = false;
  try { body(); } catch (const std::runtime_error&) { rejected = true; }
  if (rejected != expectReject) throw std::runtime_error(std::string("failed: ") + name);
  ++checks;
}
}  // namespace

int main() {
  try {
    test("C32 tail half residual", false, [] { Fixture f; nr::validateAmdFfn32(f.ffn()); });
    test("C32 E4 residual", false, [] { Fixture f; f.residual.format = nr::Format::E4; nr::validateAmdFfn32(f.ffn()); });
    test("QKV shifted tails", false, [] { Fixture f; nr::validateAmdQkv32(f.attention()); });
    test("block dual publication", false, [] { Fixture f; nr::validateAmdBlock32(f.block()); });
    test("block raw-only head anchor", false, [] { Fixture f; auto a = f.block(); a.output = nullptr; nr::validateAmdBlock32(a); });
    test("block E4-only publication", false, [] { Fixture f; auto a = f.block(); a.rawOutput = nullptr; nr::validateAmdBlock32(a); });
    test("block word-read half alignment", false, [] { Fixture f; auto a = f.block(); a.ffnScaleByteOffset = 2; nr::validateAmdBlock32(a); });
    test("synthetic aux allocation", false, [] { Fixture f; f.aux.byteLength = 0; nr::validateAmdBlock32(f.block()); });
    for (const uint32_t channels : {64u, 128u, 256u})
      test("expert family tail", false, [channels] { Fixture f(channels); nr::validateAmdExpertFfn(f.expert()); });

    test("missing operand", true, [] { Fixture f; auto a = f.ffn(); a.input = nullptr; nr::validateAmdFfn32(a); });
    test("null GPU buffer", true, [] { Fixture f; f.input.buffer.buffer = VK_NULL_HANDLE; nr::validateAmdFfn32(f.ffn()); });
    test("zero rows", true, [] { Fixture f; auto a = f.ffn(); a.rows = 0; nr::validateAmdFfn32(a); });
    test("logical row bounds", true, [] { Fixture f; f.raw.rows = 76; nr::validateAmdFfn32(f.ffn()); });
    test("allocation rows", true, [] { Fixture f; f.input.allocRows = 76; nr::validateAmdFfn32(f.ffn()); });
    test("activation allocation bytes", true, [] { Fixture f; --f.input.buffer.size; nr::validateAmdFfn32(f.ffn()); });
    test("fixed input stride", true, [] { Fixture f; f.input.channels = 64; nr::validateAmdFfn32(f.ffn()); });
    test("dual output stride", true, [] { Fixture f; f.raw.channels = 64; nr::validateAmdFfn32(f.ffn()); });
    test("wrong raw output format", true, [] { Fixture f; f.raw.format = nr::Format::F32; nr::validateAmdFfn32(f.ffn()); });
    test("F32 residual rejected", true, [] { Fixture f; f.residual.format = nr::Format::F32; nr::validateAmdFfn32(f.ffn()); });
    test("expand weights short", true, [] { Fixture f; --f.expand.size; nr::validateAmdFfn32(f.ffn()); });
    test("contract weights short", true, [] { Fixture f; --f.contract.size; nr::validateAmdFfn32(f.ffn()); });
    test("half scale alignment", true, [] { Fixture f; auto a = f.ffn(); a.scaleByteOffset = 1; nr::validateAmdFfn32(a); });
    test("scale model bounds", true, [] { Fixture f; f.aux.byteLength = 63; nr::validateAmdFfn32(f.ffn()); });
    test("scale allocation bounds", true, [] { Fixture f; f.aux.raw.size = 63; nr::validateAmdFfn32(f.ffn()); });
    test("output aliases input", true, [] { Fixture f; f.output.buffer.buffer = f.input.buffer.buffer; nr::validateAmdFfn32(f.ffn()); });
    test("dual outputs alias", true, [] { Fixture f; f.raw.buffer.buffer = f.output.buffer.buffer; nr::validateAmdFfn32(f.ffn()); });
    test("shader index overflow", true, [] { Fixture f; auto a = f.ffn(); a.rows = 0x80000000; f.input.rows = f.input.allocRows = a.rows; nr::validateAmdFfn32(a); });

    test("QKV zero geometry", true, [] { Fixture f; auto a = f.attention(); a.width = 0; nr::validateAmdQkv32(a); });
    test("QKV shift bounds", true, [] { Fixture f; auto a = f.attention(); a.shiftX = 8; nr::validateAmdQkv32(a); });
    test("QKV product overflow", true, [] { Fixture f; auto a = f.attention(); a.width = a.height = 1000000000; nr::validateAmdQkv32(a); });
    test("QKV signed coordinate overflow", true, [] { Fixture f; auto a = f.attention(); a.width = 0xffffffff; nr::validateAmdQkv32(a); });
    test("QKV source extent", true, [] { Fixture f; auto a = f.attention(); a.height = 8; nr::validateAmdQkv32(a); });
    test("QKV prior bounds", true, [] { Fixture f; --f.prior.size; nr::validateAmdQkv32(f.attention()); });
    test("QKV weights bounds", true, [] { Fixture f; --f.qkv.size; nr::validateAmdQkv32(f.attention()); });
    test("QKV f32 scale alignment", true, [] { Fixture f; auto a = f.attention(); a.scaleByteOffset = 2; nr::validateAmdQkv32(a); });
    test("QKV f32 scale range", true, [] { Fixture f; auto a = f.attention(); a.scaleByteOffset = 512; nr::validateAmdQkv32(a); });
    test("QKV output aliases prior", true, [] { Fixture f; f.output.buffer.buffer = f.prior.buffer; nr::validateAmdQkv32(f.attention()); });

    test("expert unsupported channels", true, [] { Fixture f(32); nr::validateAmdExpertFfn(f.expert()); });
    test("expert expand bounds", true, [] { Fixture f(256); --f.expand.size; nr::validateAmdExpertFfn(f.expert()); });
    test("expert narrow bounds", true, [] { Fixture f(128); --f.contract.size; nr::validateAmdExpertFfn(f.expert()); });
    test("expert projection bounds", true, [] { Fixture f(64); --f.project.size; nr::validateAmdExpertFfn(f.expert()); });
    test("expert scale bounds", true, [] { Fixture f(256); f.aux.byteLength = 511; nr::validateAmdExpertFfn(f.expert()); });

    test("block no publication", true, [] { Fixture f; auto a = f.block(); a.output = a.rawOutput = nullptr; nr::validateAmdBlock32(a); });
    test("block raw-only wrong stride", true, [] { Fixture f; auto a = f.block(); a.output = nullptr; f.raw.channels = 64; nr::validateAmdBlock32(a); });
    test("block projection bounds", true, [] { Fixture f; --f.project.size; nr::validateAmdBlock32(f.block()); });
    test("block attention aux bounds", true, [] { Fixture f; auto a = f.block(); a.attentionScaleByteOffset = 450; nr::validateAmdBlock32(a); });
    test("block uint half-load padding", true, [] { Fixture f; auto a = f.block(); a.ffnScaleByteOffset = 2; f.aux.byteLength = 66; f.aux.raw.size = 66; a.attentionScaleByteOffset = a.qScaleByteOffset = 0; nr::validateAmdBlock32(a); });
    test("block aliases skip", true, [] { Fixture f; f.raw.buffer.buffer = f.residual.buffer.buffer; nr::validateAmdBlock32(f.block()); });
    printf("AMD fusion contracts: %d CPU checks PASS (no GPU).\n", checks);
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "%s\n", e.what()); return 1;
  }
}
