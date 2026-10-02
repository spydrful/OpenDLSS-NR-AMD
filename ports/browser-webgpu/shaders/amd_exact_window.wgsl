// Diagnostic direct numerical schedule, four queries per 64-thread group.
// Append to numerics.wgsl. Shared memory is 18.5 KiB and contains f32 values
// whose narrowing/publication is explicit on the bit pattern.
struct ExactWindowParams {
  tokens: u32, heads: u32, width: u32, height: u32,
  channels: u32, shift_x: u32, shift_y: u32, use_prior: u32,
}
@group(0) @binding(0) var<uniform> ew: ExactWindowParams;
@group(0) @binding(1) var<storage, read> ew_qkv: array<u32>;
@group(0) @binding(2) var<storage, read> ew_scales: array<f32>;
@group(0) @binding(3) var<storage, read> ew_prior: array<u32>;
@group(0) @binding(6) var<storage, read_write> ew_output: array<u32>;
var<workgroup> ew_q: array<f32, 128>;
var<workgroup> ew_k: array<f32, 2048>;
var<workgroup> ew_v: array<f32, 2048>;
var<workgroup> ew_score: array<f32, 256>;
var<workgroup> ew_weight: array<f32, 256>;
fn ew_untile(t: u32) -> u32 {
  let tile = t >> 4u; let local = t & 15u;
  return ((tile >> 1u) * 4u + (local >> 2u)) * 8u + (tile & 1u) * 4u + (local & 3u);
}
fn ew_half(index: u32) -> f32 { return f16_to_f32((ew_qkv[index >> 1u] >> ((index & 1u) * 16u)) & 65535u); }
fn ew_pair_sum(row: u32, pair: u32, parity: u32) -> f32 {
  let k = row * 64u + pair * 2u + parity;
  let a = round_f16(ew_score[k] + ew_score[k + 8u]);
  let b = round_f16(ew_score[k + 16u] + ew_score[k + 24u]);
  let c = round_f16(ew_score[k + 32u] + ew_score[k + 40u]);
  let d = round_f16(ew_score[k + 48u] + ew_score[k + 56u]);
  return round_f16(round_f16(round_f16(a + b) + c) + d);
}
fn ew_softmax_sum(row: u32) -> f32 {
  var even = round_f16(ew_pair_sum(row, 0u, 0u) + ew_pair_sum(row, 1u, 0u));
  even = round_f16(round_f16(even + ew_pair_sum(row, 2u, 0u)) + ew_pair_sum(row, 3u, 0u));
  var odd = round_f16(ew_pair_sum(row, 0u, 1u) + ew_pair_sum(row, 1u, 1u));
  odd = round_f16(round_f16(odd + ew_pair_sum(row, 2u, 1u)) + ew_pair_sum(row, 3u, 1u));
  return round_f16(even + odd);
}
fn ew_norm(values: array<f32, 32>) -> f32 {
  var r: array<f32, 16>;
  for (var i = 0u; i < 16u; i++) {
    r[i] = round_f16(values[i] * values[i] + round_f16(values[i + 16u] * values[i + 16u]));
  }
  for (var stride = 8u; stride > 0u; stride >>= 1u) {
    for (var i = 0u; i < stride; i++) { r[i] = round_f16(r[i] + r[i + stride]); }
  }
  return round_f16(1.0 / sqrt(r[0]));
}
fn ew_publish(value: f32) -> f32 { return decode_e4m3(encode_e4m3(f16_bits(value))); }

@compute @workgroup_size(64)
fn amd_exact_window(@builtin(workgroup_id) group: vec3<u32>, @builtin(local_invocation_index) thread: u32) {
  let windows_x = (ew.width + ew.shift_x + 7u) / 8u;
  let windows_y = (ew.height + ew.shift_y + 7u) / 8u;
  let task = group.y + group.z * 65535u;
  let window = task / 16u; let query_base = (task % 16u) * 4u;
  if (window >= windows_x * windows_y) { return; }
  let head = group.x;
  let wx = i32((window % windows_x) * 8u) - i32(ew.shift_x);
  let wy = i32((window / windows_x) * 8u) - i32(ew.shift_y);
  let natural = ew_untile(thread);
  let x = wx + i32(natural % 8u); let y = wy + i32(natural / 8u);
  var q: array<f32, 32>; var k: array<f32, 32>; var v: array<f32, 32>;
  if (x >= 0 && y >= 0 && x < i32(ew.width) && y < i32(ew.height)) {
    let base = (u32(y) * ew.width + u32(x)) * ew.channels * 3u + head * 96u;
    for (var c = 0u; c < 32u; c++) { q[c] = ew_half(base + c); k[c] = ew_half(base + 32u + c); v[c] = ew_half(base + 64u + c); }
  }
  let qn = ew_norm(q); let kn = ew_norm(k); let scale = round_f16(ew_scales[head]);
  for (var c = 0u; c < 32u; c++) {
    ew_k[thread * 32u + c] = ew_publish(round_f16(k[c] * kn));
    ew_v[thread * 32u + c] = ew_publish(v[c]);
    if (natural >= query_base && natural < query_base + 4u) {
      ew_q[(natural - query_base) * 32u + c] = ew_publish(round_f16(round_f16(q[c] * qn) * scale));
    }
  }
  workgroupBarrier();
  for (var i = thread; i < 256u; i += 64u) {
    let query = i / 64u; let key = i % 64u;
    let prior_index = (head * 64u + query_base + query) * 64u + ew_untile(key);
    var accumulator = 0.0;
    if (ew.use_prior != 0u) { accumulator = f16_to_f32((ew_prior[prior_index >> 1u] >> ((prior_index & 1u) * 16u)) & 65535u); }
    for (var start = 0u; start < 32u; start += 16u) {
      var av: array<f32, 16>; var bv: array<f32, 16>;
      for (var c = 0u; c < 16u; c++) { av[c] = ew_q[query * 32u + start + c]; bv[c] = ew_k[key * 32u + start + c]; }
      accumulator = ada_fp8_fdpa16(av, bv, accumulator);
    }
    ew_score[i] = exp_weight(accumulator);
  }
  workgroupBarrier();
  if (thread < 4u) {
    let reciprocal = round_f16(1.0 / ew_softmax_sum(thread));
    for (var key = 0u; key < 64u; key++) { ew_weight[thread * 64u + key] = ew_publish(round_f16(ew_score[thread * 64u + key] * reciprocal)); }
  }
  workgroupBarrier();
  if (thread < 32u) {
    let query = thread / 8u; let column = (thread % 8u) * 4u;
    let natural_q = query_base + query;
    let qx = wx + i32(natural_q % 8u); let qy = wy + i32(natural_q / 8u);
    if (qx >= 0 && qy >= 0 && qx < i32(ew.width) && qy < i32(ew.height)) {
      var word = 0u;
      for (var component = 0u; component < 4u; component++) {
        var accumulator = 0.0;
        for (var start = 0u; start < 64u; start += 16u) {
          var av: array<f32, 16>; var bv: array<f32, 16>;
          for (var key = 0u; key < 16u; key++) { av[key] = ew_weight[query * 64u + start + key]; bv[key] = ew_v[(start + key) * 32u + column + component]; }
          accumulator = ada_fp8_fdpa16(av, bv, accumulator);
        }
        word |= encode_e4m3(f16_bits(accumulator)) << (component * 8u);
      }
      let index = (u32(qy) * ew.width + u32(qx)) * ew.channels + head * 32u + column;
      ew_output[index >> 2u] = word;
    }
  }
}
