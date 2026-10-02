// Diagnostic scalar F13 schedule. Concatenate numerics.wgsl before this file.
// This is deliberately independent of the optimized half-storage GEMM source
// transforms. It retains the graph's raw weight layout and publication points.
// No shader-f16, native half casts, SiLU lookup table, or cooperative matrices.
override MATMUL_FLAGS: u32 = 1024u;
override MATMUL_ROWS: u32 = 0u;
override MATMUL_K: u32 = 32u;
override MATMUL_N: u32 = 32u;
override LAYOUT_WEIGHT_BYTE_OFFSET: u32 = 0u;
override LAYOUT_BIAS_BYTE_OFFSET: u32 = 0u;
override LAYOUT_WEIGHT_MATRIX_CHANNELS: u32 = 32u;
override LAYOUT_WEIGHT_COLUMN_OFFSET: u32 = 0u;
override LAYOUT_OUTPUT_MATRIX_CHANNELS: u32 = 32u;
override LAYOUT_OUTPUT_COLUMN_OFFSET: u32 = 0u;
override LAYOUT_INPUT_MATRIX_CHANNELS: u32 = 0u;
const EXACT_HALF_OUTPUT: bool = __HALF_OUTPUT__;
const EXACT_DUAL_OUTPUT: bool = __DUAL_OUTPUT__;

struct ExactParams { words: array<vec4<u32>, 3> }
@group(0) @binding(0) var<storage, read> exact_a: array<u32>;
@group(0) @binding(1) var<storage, read> exact_b: array<u32>;
@group(0) @binding(2) var<storage, read_write> exact_out: array<u32>;
@group(0) @binding(3) var<storage, read> exact_skip: array<u32>;
@group(0) @binding(4) var<uniform> exact_params: ExactParams;
@group(0) @binding(7) var<storage, read_write> exact_dual: array<u32>;

fn exact_weight_index(k: u32, n: u32, channels: u32) -> u32 {
  let ki = k & 31u; let ni = n & 127u;
  let ng = ni & 15u;
  let lane = ((ng & 7u) << 2u) | ((ki & 15u) >> 2u);
  let byte = ((ng >> 3u) << 3u) | ((ki >> 4u) << 2u) | (ki & 3u);
  return (k >> 5u) * channels * 32u + (n >> 7u) * 4096u
    + (ni >> 6u) * 2048u + ((ni & 63u) >> 4u) * 512u + lane * 16u + byte;
}
fn exact_input_channel(k: u32) -> u32 {
  let quarter = k & 15u;
  return (k & ~31u) + (k & 16u) + (quarter >> 2u) * 2u + (quarter & 1u)
    + select(0u, 8u, (quarter & 2u) != 0u);
}
fn exact_weight_byte(index: u32) -> u32 { return (exact_b[index >> 2u] >> ((index & 3u) * 8u)) & 255u; }
fn exact_a_byte(index: u32) -> u32 { return (exact_a[index >> 2u] >> ((index & 3u) * 8u)) & 255u; }
fn exact_residual(index: u32, column: u32) -> f32 {
  if ((MATMUL_FLAGS & 2u) == 0u) { return 0.0; }
  var value: f32;
  if ((MATMUL_FLAGS & 262144u) != 0u) {
    value = decode_e4m3((exact_skip[index >> 2u] >> ((index & 3u) * 8u)) & 255u);
  } else { value = f16_to_f32((exact_skip[index >> 1u] >> ((index & 1u) * 16u)) & 65535u); }
  if ((MATMUL_FLAGS & 8u) != 0u) {
    let aux = (LAYOUT_BIAS_BYTE_OFFSET >> 1u) + column;
    value *= f16_to_f32((exact_b[aux >> 1u] >> ((aux & 1u) * 16u)) & 65535u);
  }
  return round_f16(value);
}

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) group: vec3<u32>, @builtin(local_invocation_index) thread: u32) {
  let columns = (MATMUL_N + 31u) / 32u;
  let batch = group.x / columns;
  let row_base = (group.y + group.z * 65535u) * 32u;
  let column_base = (group.x % columns) * 32u;
  let input_stride = select(LAYOUT_INPUT_MATRIX_CHANNELS, MATMUL_K, LAYOUT_INPUT_MATRIX_CHANNELS == 0u);
  let input_base = select(batch * MATMUL_K, 0u, (MATMUL_FLAGS & 131072u) != 0u);
  var k_span = MATMUL_K;
  if ((MATMUL_FLAGS & 8192u) != 0u) { k_span = 1024u; }
  if ((MATMUL_FLAGS & 16384u) != 0u) { k_span = 512u; }
  if ((MATMUL_FLAGS & 32768u) != 0u) { k_span = 256u; }
  for (var tile_word = thread; tile_word < 256u; tile_word += 64u) {
    let row = row_base + tile_word / 8u;
    let column = column_base + (tile_word % 8u) * 4u;
    if (row >= MATMUL_ROWS || column >= MATMUL_N) { continue; }
    let output_index = row * LAYOUT_OUTPUT_MATRIX_CHANNELS + LAYOUT_OUTPUT_COLUMN_OFFSET + batch * MATMUL_N + column;
    var results: array<f32, 4>;
    for (var component = 0u; component < 4u; component++) {
      let col = column + component;
      var accumulator = exact_residual(output_index + component, col);
      var total = 0.0;
      for (var start = 0u; start < MATMUL_K; start += 16u) {
        var av: array<f32, 16>; var bv: array<f32, 16>;
        for (var i = 0u; i < 16u; i++) {
          let k = start + i;
          let channel = select(k, exact_input_channel(k), (MATMUL_FLAGS & 16u) != 0u);
          av[i] = decode_e4m3(exact_a_byte(row * input_stride + input_base + channel));
          let offset = LAYOUT_WEIGHT_BYTE_OFFSET + batch * MATMUL_K * LAYOUT_WEIGHT_MATRIX_CHANNELS
            + exact_weight_index(k, col + LAYOUT_WEIGHT_COLUMN_OFFSET, LAYOUT_WEIGHT_MATRIX_CHANNELS);
          bv[i] = decode_e4m3(exact_weight_byte(offset));
        }
        accumulator = ada_fp8_fdpa16(av, bv, accumulator);
        if ((MATMUL_FLAGS & 57344u) != 0u && (start + 16u) % k_span == 0u) {
          total = select(round_f16(total + accumulator), accumulator, start < k_span);
          accumulator = 0.0;
        }
      }
      if ((MATMUL_FLAGS & 57344u) != 0u) { accumulator = total; }
      if ((MATMUL_FLAGS & 4u) != 0u) { accumulator = mp_cubic_silu(accumulator); }
      results[component] = accumulator;
    }
    let h0 = f16_bits(results[0]); let h1 = f16_bits(results[1]);
    let h2 = f16_bits(results[2]); let h3 = f16_bits(results[3]);
    if (EXACT_HALF_OUTPUT) {
      exact_out[output_index >> 1u] = h0 | (h1 << 16u);
      exact_out[(output_index >> 1u) + 1u] = h2 | (h3 << 16u);
    } else {
      exact_out[output_index >> 2u] = encode_e4m3(h0) | (encode_e4m3(h1) << 8u)
        | (encode_e4m3(h2) << 16u) | (encode_e4m3(h3) << 24u);
    }
    if (EXACT_DUAL_OUTPUT) {
      exact_dual[output_index >> 1u] = h0 | (h1 << 16u);
      exact_dual[(output_index >> 1u) + 1u] = h2 | (h3 << 16u);
    }
  }
}
