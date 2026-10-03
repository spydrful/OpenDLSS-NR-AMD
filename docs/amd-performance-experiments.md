# RX 9070 XT experiments after alpha 4

These development experiments investigate FP8 GEMM tiling, accumulator
initialization and SiLU publication on the RX 9070 XT. They retain the native
Vulkan graph, K16 accumulation publication, model loader and D3D12 bridge.
The final epilogue candidate improves ordinary target network median by
**3.9344%**, from **58.668700 to 56.360420 ms** against the frozen alpha 4 policy.
It passes the completed strict comparisons but falls below the **5% default
promotion gate**. The published default, tuning cache and release remain alpha 4;
NR remains disabled by default. No alpha 5 publication follows from this result.

The baseline is the published
[alpha 4 source at `df300456a2497fa4af6cedccc440c48eea56daa8`](https://github.com/spydrful/OpenDLSS-NR-AMD/tree/df300456a2497fa4af6cedccc440c48eea56daa8),
its [release identities](performance/alpha4-release-identities.json) and the
[RTE delivery record](amd-rte-delivery.md). Its qualified target policy uses
Direct-RTE/Register-RTE, K16 publication, N16/stage16 GEMM and Q32 attention,
with fusion and packed publication disabled. Published alpha 1–4 artifacts
and their measurements remain unchanged.

The separate
[post-alpha-4 experiment evidence](performance/post-alpha4-gemm-experiments-rx9070xt-20261003.json)
binds the completed comparisons to their actual executable, shader modules,
source, driver, model and policy. Network GPU timestamps exclude the D3D12 bridge,
FSR, game and presentation. They establish no new game FPS or NR-plus-bridge
budget result.

## Final ordinary timing

The final third-revision CLI uses `direct-rte-epilogue`, K16 publication,
N16/stage16 and Register-RTE Q32 attention. All fusion and packed-publication
experiments remain off. Valid input is **1707×960**, padded to **1728×960**, on
Windows 11, RX 9070 XT, Adrenalin **26.9.1 (LLPC)** and Vulkan **1.4.349**.

| Ordinary network GPU timing | Frozen alpha 4 | Final epilogue candidate |
| --- | ---: | ---: |
| Median | 58.668700 ms | 56.360420 ms |
| P95 | 58.984608 ms | 56.686794 ms |
| P99 | 59.118213 ms | 56.791940 ms |
| Mean | 58.661181 ms | 56.347636 ms |
| Standard deviation | 0.225627 ms | 0.213311 ms |
| Coefficient of variation | 0.3846% | 0.3786% |

Three interleaved pairs each use five warmup frames and 30 measured frames,
yielding **90 retained samples per condition**. Image readback and per-dispatch
instrumentation are off. Candidate/baseline ratios are **0.960655682 median** and
**0.961043837 P95**: improvements of **3.9344%** and **3.8956%**, respectively.
The no-greater-than-2% complete-inference regression gate passes, while the
at-least-5% default improvement gate fails. The candidate remains an explicit
development selection.

The final CLI SHA-256 is
`f8cb189a7c8ae97c6d2f6ced5fef644f2adbd73ceaa88c012ca1bb4d38fdb845`;
its selected shader identity is
`9929119ef659f9f389bcd26c0396567e609efdb5ebd32a90c9c20e5a66a72026`.
The unchanged baseline CLI and selected shaders are
`f29be529e14a4b896b7ab4424d7618b5b6dce3aa6c8d2156f17fca6fdfca1f0f`
and `63c5e7decfeac23900f1d98a9b1201b79a91e0dcf550079ee1cd0f9b1cd02c96`.
The public scalar record carries the remaining provenance and raw measurement
hashes. Rebuilding source does not imply identical EXE or DLL bytes.

Separate final profiles retain the actual frame samples and dispatch metadata.
They identify 13 of 46 FP8 GEMM groups meeting the operator performance gate;
the whole-network gate still uses the ordinary runs above. Independently
minimized dispatch sums remain separate from measured frame totals.
Installed-driver pipeline executable statistics also cover actual target
geometry and selected modules. Resource statistics are separate from offline
RGA and RGP captures. A separate installed-driver `ISA.cs` query confirms
`v_wmma_f32_16x16x16_fp8_fp8` in all **32 compiled FP8 GEMM variants** at this
geometry. The evidence records static instruction sites, which do not measure
executed instruction counts or utilization. Pipelines require wave32; the
driver's executable-statistics `subgroup 128` field is reported verbatim and
does not establish wave128 execution. This inspection does not establish the
cause of the rejected N32 or earlier SiLU discrepancies.

## What the screens have established

| Experiment | Observed strict result | Development decision |
| --- | --- | --- |
| Global N32 using the frozen Direct-RTE module | 162 of 862 byte checks fail, including 18,057 differing bytes in E4 overflow cases | Reject as a preserving replacement despite the complete timing protocol's 7.0744% median improvement |
| Constant-zero accumulator initialization, first revision | 161 failed checks at N16 and 324 at N32 | Reject both variants |
| Runtime-zero initialization, second revision | All 862 operator checks pass; the 320×320 model first differs at block 31 | Operator coverage is insufficient to promote this variant |
| Runtime-zero initialization limited to non-residual operators with K ≤512, third revision | Initialization-only and corrected epilogue variants each pass the extended operator suite, 320×320 checkpoints and target head/proxy/capture comparisons | Retain explicit development routes; final epilogue timing misses default promotion |

The rejected N32 timing uses the full three-pair, five-warmup, 30-measured-frame
protocol: **59.189400 → 55.002080 ms median**. Its strict failures prevent preserving
promotion. Earlier short screens remain separately labeled in the evidence.

The limited initialization path retains the original shared-memory seed for
residual operators and K >512. Its shortcut receives the positive-zero bit
pattern through a uniform push constant, rather than a compile-time matrix
constructor. Ordered K16 operations, partition reduction and publication
locations remain unchanged. The second revision's model failure is retained as
evidence that the original operator suite did not cover every model condition.

The global N32 profile identifies operator improvement in only 19 of 46 measured
GEMM groups. The current cache cannot route a separate GEMM module for each shape.
Any future mixed selector therefore requires an explicit routing implementation
and fresh complete-inference and strict-output qualification. A collection of
operator winners does not qualify a global policy.

## Corrected SiLU publication

The candidate epilogue uses a preserving mixed implementation of
`common.glsl::mpCubicSilu`. It retains the original F32 operation order, explicit
FMAs, clamp, constants and all five half-publication boundaries:

| Boundary | Corrected publication |
| --- | --- |
| Clamp to [−4, 4] | Scalar half RTE |
| Absolute value | Scalar half RTE |
| Inner F32 FMA | Original software half publication |
| Polynomial F32 FMA | Scalar half RTE |
| Final F32 multiply | Original software half publication |

The initial attempt replaced all five software publications with scalar RTE.
It changed 12 inner-FMA checkpoints and 196,588 final-multiply checkpoints;
189,876 E4 outputs differed. Adding complete F16 and F32 signed-zero/Inf/NaN
execution modes reproduced those failures exactly. Declarations alone did not
establish the prescribed arithmetic on this compiler and driver.

For input F32 bits `3cc4c000`, the exact inner FMA lies `1/134217728` above a
half midpoint. The prescribed F32 FMA first rounds to that midpoint, then half
RTE publishes `3ee44000`. The failed candidate published `3ee46000`. Compiler
narrowing is an inference from this witness; installed-driver ISA was not
inspected. The final multiply also lost negative zero in the failed candidate.
The corrected helper keeps the original software publication at those two
boundaries and uses scalar RTE at the other three.

The corrected isolated GPU diagnostic passes **653,283 distinct F32 inputs**:
every **65,536 half bit pattern** plus **587,747 additional witnesses**. It compares
raw bits at every old/candidate checkpoint, the actual helper results, direct
publication and E4 output. An independent CPU audit recomputes the raw-buffer
results, identities, corpus coverage and integer conversion rules. The CPU
harness passes **446,477 checks**. This corpus includes half midpoints and their
F32 neighbors, final-output midpoint witnesses, clamp and conversion boundaries,
signed zeros, nonfinite values and seeded F32 inputs. It is not exhaustive F32
coverage and does not qualify the production GEMM epilogue by itself.

All inputs, including nonfinite half patterns, remain strict GPU old/candidate
comparisons. Finite-input checkpoints and E4 codes also compare to the CPU
oracle. CPU NaN payloads after clamp/FMA/multiply are not used as a raw SiLU
oracle; direct nonfinite publication must preserve the original F32 bits.
Signed-zero differences receive no exemption.

The epilogue requires supported half RTE, denormal and signed-zero/Inf/NaN
controls, compatible control independence, and F32 signed-zero/Inf/NaN
preservation. The tested glslang intrinsic retains only one width when declaring
the same execution mode twice. A structural postcompile closure restores both
widths and fails closed for malformed or unsupported modules. Its 68 CPU checks
cover exact insertion, instruction preservation, atomic replacement and
idempotence. The actual diagnostic module contains all required F16/F32 modes.
These checks do not claim that a SPIR-V semantic validator ran. The installed
Vulkan validation layer was absent.

## Qualification and measurement protocol

Each experiment receives an isolated build directory. Freeze source and module
hashes, executable, model manifest, compiler, driver, geometry, arithmetic and
actual selected variants before measuring. Keep old rejected artifacts and raw
results. Performance collection and instrumented profiling use separate runs;
ordinary timing has image readback and dispatch instrumentation disabled.

Initialization-only and corrected epilogue variants each pass **674 operators /
880 strict byte checks / 14,332,928 bytes**, with zero mismatches. Coverage includes
tails, broadcasts, residuals, partitions, activation, padding, half overflow and
subnormals, signed zeros and E4 saturation. Fourteen added ViT fixtures exercise
K1024/K4096, partitions, residuals, broadcasts and activation. They extend the
suite after the second revision passed its original operator tests but failed
the model.

At **320×320**, both variants match all **75 model checkpoints**, the F32 head,
composition proxy and production/capture head byte for byte. At target geometry,
both match the **head, composition proxy and production/capture head**; zero
intermediate boundaries were compared there. These use identical synthetic reset
features. The composition proxy is clamped, truncated-half display-proxy RGB,
so these comparisons do not establish scene-linear game quality. Selected
production captures are recorded separately. Fresh final-CLI epilogue and frozen
baseline runs also use the decomposed `--intermediates` route. The formal
preserving model proof passes **77 checks at 320×320**: all 75 boundaries, the
head and bundled baseline/candidate capture-production checks. Its target proof
passes **two checks**, covering head and capture-production. These synthetic
proofs apply no image-quality or performance qualification threshold.

The final epilogue replay passes **80 byte checks / 2,107,883,520 bytes** over eight
existing genuine SDR game frames, in both identical and independently evolved
histories. It reuses the frozen alpha 4 replay reference and compares the head,
published history, runtime head and scene-linear RGBA buffers exactly. The
matched RGB frames are exact, with SSIM 1.0 and infinite PSNR against that AMD
reference across **16 frame/history comparisons**. Identical history uses the
captured ancestry; independently evolved history starts from reset and then
uses each variant's own preceding outputs. Evolved replay therefore does not
claim the original captured ancestry. This bounded replay adds no new game
capture, broad motion review or original NVIDIA parity.

The native runtime harness passes at **320×320 and 1707×960 with eight requested
production frames**. Those requested frames run sequentially. Its separate
eight-slot ownership tests at **320×320** prepare simultaneous slots and check
ordered/prefetched output equality, cancellation, gap/reset, history ancestry
and descriptor lifetime. Target-resolution simultaneous eight-slot ownership
is not claimed. Fence/shared-buffer round trips, resource-state continuation,
failure recovery, drain and resize also pass. Harness readback and tracing runs
provide lifecycle evidence; their timestamps do not qualify performance.

The full ordinary timing protocol uses five warmup frames, followed by three
interleaved baseline/candidate pairs of 30 measured frames each. Report median,
P95, P99 and variation over actual per-frame samples. A retained operator variant
needs at least 5% operator improvement and no greater than 2% complete-inference
median or P95 regression. An optimized default needs at least 5% complete target
inference improvement in addition to all preserving gates. Short screens and
profile sums remain separately labeled.

Production promotion also requires the remaining applicable synthetic SDR/HDR,
quality and game checks. Changed arithmetic experiments remain separate and opt-in;
their composed-frame quality gate is PSNR ≥40 dB and SSIM ≥0.99 on every matched
unclamped scene-linear RGB frame with fixed data range 1.0. Existing broad
temporal review and HDR-highlight quality limits remain outstanding.

## Reproduce the development route

Use the Windows toolchain prerequisites in [AMD.md](AMD.md#build-and-local-checks).
Extract the unchanged alpha 4 package locally and use your own imported model.
Build current experimental source in a new directory; this does not alter the
installed game or previously released binaries:

```powershell
$candidate = 'D:\amd\build\experiments\post-alpha4'
$alpha4 = 'D:\local\OpenNR-AMD-alpha4'
$model = 'D:\local\open-nr\model'
./scripts/build.ps1 -Backend amd -OutputDirectory $candidate
./scripts/build_game.ps1 -SkipCore -OutputDirectory $candidate
./scripts/build_interop.ps1 -OutputDirectory $candidate
```

Force the tested epilogue route for diagnostics. `direct-rte-init` selects the
initialization-only experiment; `direct-rte` selects the published baseline
module. Use new absent fixture directories and keep all output under `build/`:

```powershell
$policy = @(
  '--amd-kernels', 'optimized', '--amd-arithmetic', 'k16',
  '--amd-gemm', 'direct-rte-epilogue', '--amd-tile-n', '16', '--amd-stage-k', '16',
  '--amd-window-layout', 'register-rte', '--amd-window-queries', '32',
  '--amd-fusion', '0', '--amd-ffn32-fusion', '0', '--amd-qkv32-fusion', '0',
  '--amd-expert-fusion', '0', '--amd-block-fusion', '0', '--amd-hardware-publication', '0'
)
& "$candidate\dlss5vk.exe" amdcheck --backend amd `
  --baseline-shaders "$alpha4\payload\open-nr\shaders" --shaders "$candidate\shaders" `
  --fixture 'D:\amd\build\experiments\post-alpha4-operators' `
  --comparison-anchor rte32 @policy
```

The `rte32` comparison anchor means the frozen Direct-RTE **N16** / Register-RTE
**Q32** policy. It names the attention geometry, rather than a global N32 GEMM
experiment. Generate a frozen baseline model fixture using the alpha 4 CLI,
then require exact candidate output with decomposed capture:

```powershell
$baselineModel = 'D:\amd\build\experiments\post-alpha4-baseline320'
$candidateModel = 'D:\amd\build\experiments\post-alpha4-candidate320'
$baselinePolicy = $policy.Clone()
$baselinePolicy[5] = 'direct-rte'
& "$alpha4\tools\dlss5vk.exe" modelcheck --backend amd --model $model `
  --shaders "$alpha4\payload\open-nr\shaders" --fixture $baselineModel `
  --width 320 --height 320 --frames 3 --intermediates @baselinePolicy
& "$candidate\dlss5vk.exe" modelcheck --backend amd --model $model `
  --shaders "$candidate\shaders" --fixture $candidateModel --reference $baselineModel `
  --width 320 --height 320 --frames 3 --intermediates --require-exact @policy
```

Repeat at `--width 1707 --height 960 --head-only` with two new fixture paths for
target head/proxy/capture scope. The identity-bound formal model tool is
`tools/qualify_amd_model.py`; it also requires the matching baseline/candidate
benchmark JSON and the `rte32` anchor. Captures and exported activations stay
private.

Run the full ordinary timing comparison on an otherwise idle GPU:

```powershell
./scripts/benchmark_amd.ps1 -Executable "$candidate\dlss5vk.exe" `
  -ShaderDirectory "$candidate\shaders" `
  -BaselineExecutable "$alpha4\tools\dlss5vk.exe" `
  -BaselineShaderDirectory "$alpha4\payload\open-nr\shaders" `
  -ModelDirectory $model -OutputDirectory 'D:\amd\build\experiments\post-alpha4-bench' `
  -ComparisonAnchor rte32 -Kernels optimized -Arithmetic k16 `
  -Gemm direct-rte-epilogue -TileN 16 -StageK 16 `
  -WindowLayout register-rte -WindowQueries 32 -Ffn32Fusion 0 -Qkv32Fusion 0 `
  -Width 1707 -Height 960 -Warmup 5 -Frames 30 -Pairs 3
```

Repeat with `-Mode profile` and another new output path for instrumentation.
Do not run readback captures or profiles concurrently with ordinary timing.
Bind the decomposed model fixtures to the matching ordinary run identities:

```powershell
python ./tools/qualify_amd_model.py --baseline $baselineModel --candidate $candidateModel `
  --baseline-benchmark 'D:\amd\build\experiments\post-alpha4-bench\pair-01-baseline.json' `
  --candidate-benchmark 'D:\amd\build\experiments\post-alpha4-bench\pair-01-candidate.json' `
  --comparison-anchor rte32 `
  --output 'D:\amd\build\experiments\post-alpha4-model320-qualified.json'
```

Use `--target-only` with the separate target fixtures for the formal target
head/capture proof. Each output is a new private proof path.
`scripts/probe_amd_silu.ps1` defaults to CPU-only checks; `-RunGpu` explicitly
dispatches the standalone diagnostic. Native session selection also accepts
`DLSS5VK_AMD_GEMM=direct-rte-init` or `direct-rte-epilogue`, read at session
creation. These forced development selections require new qualification after
a source, shader, driver or policy change; they are not instructions to promote
the default cache.

The outstanding complete Cyberpunk protocol includes three warmed NR-off/NR-on
benchmark passes and ten minutes of active gameplay, with frame generation and
AFMF off. Complete game frame times, inference and bridge timings, VRAM and
bypasses require their own measurements. These experiments establish neither the
8 ms NR-plus-bridge budget nor the 16.67 ms / 60 real FPS goal. Generated weights,
NVIDIA DLLs, model buffers and game captures remain outside public evidence and
release packages.
