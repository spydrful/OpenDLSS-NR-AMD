# AMD arithmetic validation

The AMD and reference backends reuse the fork's original 71-block graph, tensor
layout, model loader, and half/E4M3 publication points. NVIDIA PTX launches,
counter chaining, and fused NVIDIA shaders are disabled for these backends.

`--backend reference` emulates the documented Ada F13 dot product in groups of
16 FP8 products, and F24 dot product in groups of eight half products. The
reference window and global attention kernels preserve the fixed softmax
reduction order, padded-key correction, and half/E4 publications. This is a
verification path, not a performance target.

`--backend amd` uses queried Vulkan KHR 16×16×16 E4M3 matrices with f32
accumulators and a required subgroup size of 32 for FP8 GEMMs and window
attention. It publishes each group of 16 products to half and retains split-K
publication order. AMD's f32 matrix arithmetic does not implement Ada's
per-product F13 alignment and truncation, so this route is deliberately not
advertised as bit-exact. Global attention uses a cooperative matrix kernel
with bounded 64-key staging and half publication after each group of 16.
`DLSS5VK_AMD_GLOBAL_SCALAR=1` selects the scalar f32 global dot-product kernel
for an explicit comparison. The half adapter/head uses the exact software F24
kernel on both routes. The first implementation keeps graph
intermediates in GPU buffers and uses Vulkan barriers between dispatches;
fusion and further performance tuning remain work to be measured.

On the development RX 9070 XT, both backends passed the model-free operator,
all-half E4 publication, and optional game-shader checks. Both also completed
the generated 320×320 graph with 513 dispatches and 409,468 nonzero finite
head samples. Separate scratch, reused scratch, and a re-recording produced
identical head bytes within each backend. This compares allocation schemes
within a backend; it does not compare the AMD optimized graph with the
reference graph or the real NVIDIA model.

## Model-free tests

Run from the repository root after building:

```powershell
./build/dlss5vk.exe selftest --backend reference
./build/dlss5vk.exe selftest --backend amd
node ./ports/browser-webgpu/tools/check_numerics.mjs
```

The native GPU test covers:

| Case | Comparison and scope |
| --- | --- |
| Half to E4M3 | All 65,536 half bit patterns; exact GPU bytes versus a separate integer transcription of the WebGPU encoder and the C++ encoder. Literal witnesses cover signed zero, NaN to +0, positive/negative infinity and finite saturation, and subnormal RNE ties. |
| FP8 GEMMs | Batch offsets, broadcast inputs, residual seeding and scaling, split-K, SiLU, f16/E4 dual publication, and a 67-row tail. Reference results compare bytes/bits; AMD results report differences and a coarse error bound. |
| Half adapter/head | 67 rows of F24 dot products; exact f32 publication bits on both backends. |
| Window normalization | Two heads, 67 tokens, and an all-zero row exercising NaN-to-zero publication. The CPU helper returns decoded floats, so signed zeros compare equal in this case. |
| Window attention | Learned priors, physical key order, and shifted windows with out-of-bounds keys. The CPU helper returns decoded floats, so signed zeros compare equal in this case. |
| Global attention | Two heads with 65 valid tokens and 63 padding keys; exact E4 bytes for reference against a separate CPU schedule transcription. This is not an established native capture. |
| Game frame shaders, when built | Actual preprocess/composite SPIR-V: feature/control layout, reflected padding, reset/temporal history priming, bypass bits and alpha, and neutral SDR/HDR exposure reversal. Transcendentals use numerical tolerances; this is not real game validation. |

The JS check compares the existing WebGPU arithmetic transcription against the
repository's numeric fixture: exhaustive half-to-E4 encoding, E4 decoding,
finite-half SiLU and window exponentials, arbitrary f32-to-half patterns, and
random FP8/F24 dot products. It is a CPU/JS cross-check. Running that command
does not execute WGSL in a browser, and the fixture's origin is the upstream
Vulkan CPU reference rather than an independent AMD or NVIDIA full-model run.

The AMD synthetic error bound catches incorrect matrix layouts and gross
arithmetic errors. It is not a release quality threshold, an image-quality
measurement, or proof of temporal stability. The random operator cases use
small finite operands; exhaustive special-value coverage is confined to the
publication conversion. Nonfinite matrix operands and arbitrary pathological
model weights are not certified by these tests.

An optional generated full-graph smoke test exercises the original tensor
loader and all 71 blocks with sparse deterministic nonzero weights and patterned
features. It compares separate scratch allocations with reused scratch and a
re-recording, requiring exact same-backend head bytes and finite nonzero output.
It writes an explicitly synthetic model under the build directory; it contains
no NVIDIA assets. This test establishes layout, dispatch, and scratch-lifetime
regression evidence for that generated input, not equivalence to the real model.

```powershell
$env:OPEN_NR_GRAPH_SELFTEST = '1'
./build/dlss5vk.exe selftest --backend amd
Remove-Item Env:OPEN_NR_GRAPH_SELFTEST
```

## Working memory

Native recordings without boundary/intermediate captures reuse FFN, QKV,
normalization, and attention scratch across the encoder/decoder stages of the
same shape and across full-resolution blocks 0 and 70. States, transitions,
encoder skips, retained full-resolution block 0, and the head remain separate.
Capture modes retain the original scratch allocation scheme. Set
`DLSS5VK_NO_SCRATCH_REUSE=1` or `Graph::Options::reuseScratch=false` to compare
the original scheme. Each native operator dispatch has a Vulkan barrier before
a later operator can overwrite its scratch.

For a 1707×960 render rectangle, the original geometry rule produces a
1728×960 full field and levels 864×480, 432×240, 216×120, 108×60, 56×32,
and 28×16. Static accounting of the graph's requested activation buffers,
including its input features, gives:

| Scheme | Buffer count | Requested MiB |
| --- | ---: | ---: |
| Separate stage scratch | 132 | 3513.08 |
| Reused stage scratch | 92 | 2222.88 |
| Reduction | 40 | 1290.20 |

The generated 320×320 GPU smoke test measured 131 separately allocated graph
buffers totaling 213.355 MiB and 91 reused buffers totaling 132.840 MiB, with
external input features excluded. These are the buffers' requested sizes,
rather than a driver-reported VRAM measurement.

These are calculated allocation sizes, not measured VRAM consumption. The
largest remaining full-resolution scratch allocations are QKV (303.75 MiB),
FFN (202.5 MiB), and normalized QKV (151.875 MiB). Actual VRAM also includes
raw model tensors, cached relaid-out weights/priors, Vulkan staging and dummy
buffers, allocator alignment, history, shared bridge resources, private output,
the game, and the upscaler. `Graph::activationBytes()` reports requested graph
buffer sizes after recording and excludes input allocated outside the graph.
Scratch reuse reduces residency, not operator memory traffic or dispatch count.
Fusion and the full-resolution QKV/attention round trips remain important
performance work; no frame-time improvement is implied by this accounting.

## Frame composition

The game shaders preserve the upstream feature layout, centered proxy and
history values, Gaussian seed sequence, ordered head-residual FMAs, half
truncation of history/style/intensity publications, five-tap Catmull-Rom
history reconstruction, and luminance/color-strength tone upgrade. Disabled
model display copies the original scene color and primes history with the
proxy. Exposure is applied for neural input and reversed for output. The
output remains scene-linear HDR with the original alpha; display ACES/sRGB
encoding belongs to the game.

The buffer implementation of bilinear history sampling and AMD transcendental
instructions have not been shown bit-identical to the upstream texture
sampler and NVIDIA transcendental instructions. Private game output is
RGBA16F, so scene values outside the finite half range are not representable
even when the input is RGBA32F. Camera cuts, exposure transitions,
disocclusion, game motion conventions, and HDR suitability need real matched
frame sequences.

No locally imported model or full-model reference capture was available for
the initial implementation. The supplied SF-v2 container was subsequently
inspected without executing it: its `WEIGHTS_HT` resource is byte-identical
to the original documented 310.8.0 resource (SHA-256
`836f445d06ecd2e59bb9f17b84b91c143396fd76ccda1c9dc7fe81d5edd548f4`).
The [pinned public extraction metadata](https://huggingface.co/inarikami/dlss5-nr-reverse-engineering/blob/2f3db2562d18f750169c85ed947dc15bffca5a3b/docs/nr-model-access.md)
ties that resource to the original DLL hash. No proprietary model data was
downloaded for that comparison.

`scripts/validate_model.ps1` imports no data and executes no NVIDIA DLL. It
runs the locally imported model on identical deterministic f32 feature bits,
exports all 75 boundaries and the head, and checks production replay and
capture equality. Its input is a fixed-point colour pattern with reproducible
12-uniform noise, reset history and fixed conditioning, not a captured game
frame. The composed RGB metric is clamped, truncated-half display-proxy RGB
with style 0, intensity 1 and no history feedback, before the game HDR tone
transfer. It reports AMD/reference differences without applying a visual
quality threshold. The direct WGSL diagnostic independently compares the
reference exports; their producer is explicitly the local Vulkan software
route, not NVIDIA's runtime. `-OptimizedWebGpu` instead runs the original
browser implementation, whose Radeon mismatch is documented below.
`--require-exact` is available on
`dlss5vk modelcheck` when bitwise equality is the intended gate.

Whole-model NVIDIA capture parity and optimized still/temporal quality remain
unverified. End-to-end game performance has been measured and misses the
requested performance targets by a wide margin; see the
[RX 9070 XT validation record](rx9070xt-validation.md) for its scope and results.
See [AMD.md](AMD.md) for the remaining release gates and measurement tools.

## RX 9070 XT diagnostic evidence

The locally supplied real model was run at 320×320 on identical feature bytes
with both native backends. Production replay, re-recording and capture-mode
head bytes agree within each backend. The accelerated route differs from the
F13 reference: the reset display-proxy RGB comparison gives RMSE
0.00538409 and maximum error 0.04498291 (45.38 dB for an explicit unit range).
That metric is a deterministic diagnostic input, not a game-quality approval.

Coalesced wave32 normalization retains the original half reduction tree and
is bit-exact to the earlier accelerated implementation in all 75 real-model
boundaries and the head. The cooperative global kernel stages 64 keys at a
time, preserves padded-key denominator subtraction and keeps shared memory
independent of token count. Its padded/tail synthetic tests pass, and all 75
320×320 boundaries and the head are bit-exact to the earlier accelerated
scalar global kernel. `DLSS5VK_AMD_GLOBAL_SCALAR=1` selects that scalar kernel
for an explicit comparison.

On the idle local RX 9070 XT, the final native inference benchmark measured
23.570 ms median at 320×320 and 217.949 ms median at valid 1707×960
(1728×960 padded field). The target profile measured global attention at
2.115 ms; full-resolution window-attention passes remain about 17.2–17.5 ms
each. Those inference timings exclude game bridge, upscaling and display.
The 8 ms target is not met. Earlier higher measurements overlapped a game's
menu rendering and should not be treated as a clean optimization baseline.

## Measured next work

These priorities follow the local RX 9070 XT target profile in
`build/interop/global-matrix-profile1707.log`, which reports 210.206 ms as the
sum of dispatch spans. They are proposed experiments, not measured speedups.
The separate warmed inference median is 217.949 ms; reaching 8 ms requires
more than a 27-fold reduction before adding the bridge or game.

1. **Window attention.** The listed window-attention kernels total about
   108.164 ms, or 51.5% of the profiled spans. The two full-resolution passes
   cost 17.479 and 17.234 ms. Inspect compiled LDS/register occupancy, reduce
   duplicated score storage, and distribute softmax/publication work across
   the subgroup while preserving the existing half reduction tree, learned
   prior order, padded keys, and K16 publications.
2. **FP8 GEMM reuse.** The current 64-row by 16-column tile reloads input for
   each 16-column group and uses two workgroup barriers per K16 step. Evaluate
   wider output tiles and larger staged K loads with the same K16 matrix and
   half-publication sequence. Tail rows, residual seeding, SiLU, dual outputs,
   broadcasts, and split-K reduction order must remain regression gates.
3. **Full-resolution intermediate traffic.** Evaluate fused QKV,
   normalization, attention, and projection paths to reduce buffer round trips
   and residency while retaining the unfused capture path. Dispatch removal
   alone is a small opportunity: 400 trivial dispatches plus barriers measured
   only 0.385 ms. The exact F24 adapter and head cost 1.683 and 1.493 ms in this
   profile, so replacing their arithmetic is not the first priority.
4. **Accelerated HDR accuracy.** Localize fast/reference error in matched
   composed frames before selecting different publication or mixed-precision
   arithmetic. The synthetic HDR reset case fails PSNR 40 dB, and the temporal
   case also fails SSIM 0.99. Performance improvements do not satisfy these
   separate quality gates. Preserve fixed data range, actual histories and
   controls, and the independent exact reference.

Each candidate needs same-backend boundary/head regression checks, matched
scene-linear RGB comparisons, and otherwise idle warmed target measurements.
Deeper frame buffering addresses preparation capacity; it does not reduce the
network's GPU work or establish a frame-time improvement.

## Independent WGSL diagnostic

The original optimized WebGPU implementation on this Radeon does not match
the reference export. Block 0's F24 adapter is exact, but its optimized SiLU
lookup table differs from the independent JavaScript publication oracle in
14,481 raw-half and 14,574 E4 entries among 63,488 finite half inputs.
`ports/browser-webgpu/web/amd_block0.html?exact=1&exactWindow=1` uses separate
direct WGSL F13/explicit-publication kernels. All nine block-0 intermediate
stages are bit-exact against the Vulkan software reference with the actual
model. The full direct diagnostic also passes all 75 E4 boundary buffers and
the 409,600-value f32 head exactly. Its GEMM loop shapes are read from the
recorded uniforms to keep the graphics driver from spending minutes compiling
large scalar K specializations. The optimized browser remains unchanged. The direct diagnostic is
independent graphics-API evidence for the numerical schedule, not parity with
NVIDIA's proprietary runtime or a performance implementation.

```powershell
$env:NR_WEIGHTS = "$PWD/models/imported/open-nr"
$env:NR_FIXTURES = "$PWD/build"
$env:CHROME = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
node ports/browser-webgpu/tools/headless.mjs block0 'fixture=/fixtures/real-model-reference320&exact=1&exactWindow=1'
# Full-graph direct diagnostic; requires the corresponding modelcheck export.
node ports/browser-webgpu/tools/headless.mjs direct 'fixture=/fixtures/real-model-reference320'
```
