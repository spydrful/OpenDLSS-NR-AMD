# RX 9070 XT experiments after alpha 4

These development experiments investigate FP8 GEMM tiling, accumulator
initialization and SiLU publication on the RX 9070 XT. They retain the native
Vulkan graph, K16 accumulation publication, model loader and D3D12 bridge.
The historical third-revision epilogue candidate improved ordinary target
network median by
**3.9344%**, from **58.668700 to 56.360420 ms** against the frozen alpha 4 policy.
It passed the then-current 880-check suite but failed a subsequently added
partition case described below. Its timing also falls below the **5% default
promotion gate**. The qualified auto policy and tuning cache remain from alpha 4;
NR remains disabled by default. These historical results do not qualify the
current repaired modules.

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
budget result. The [paired-GEMM and attention-arena continuation](amd-pair-arena-delivery.md)
and [separate scalar evidence](performance/pair-arena-rx9070xt-20261003.json)
track the repaired source and new named experiments. Earlier public JSON and
its hashes remain unchanged.

## Expanded-suite erratum

The expanded **724-operator / 934-check** suite adds paired-preload boundaries,
eight-batch GEMMs and thin-window padding. It exposes a valid partitioned case
missing from the earlier suite: **K192, N48, 63 rows, flags 0, partition 96,
two batches**. The historical third-revision Init and Epilogue modules and the
initial Pair module each differ from alpha 4 by **10,477 bytes** in that case.
The prior 880-check, model and replay matches remain accurate for their tested
inputs; they are insufficient evidence for this broader operator contract.

The repair retains the original shared-memory accumulator initializer for
every partitioned operator. The runtime-zero shortcut is now restricted to
**non-residual, unpartitioned K ≤512** operators. Ordered K16 matrix operations
and publication locations stay in place. Current Init, Epilogue and Pair modules
require fresh expanded operator and model evidence; their new source and shader
identities cannot inherit the historical proof. The published alpha 4 Direct-RTE
module, Register-RTE attention, qualified cache and release are unchanged.

Current qualification tools require the new paired-preload regression evidence
for **Init, Epilogue and Pair** operator proofs. Older 880-check experimental
reports stay archived with their original hashes but are rejected as
insufficient for new qualification. Earlier published Direct-RTE evidence and
the alpha 4 default cache retain their existing contract. Arena proofs also
require executed thin-window padding cases and full output allocations.

The new explicit `direct-rte-pair` selector stages two ordered K16 operand
fragments for K >128 and currently requires N16/stage16. `arena-rte` reuses shared
storage for scores, weights and output scratch and supports Q16/Q32. Its declared
per-workgroup shared storage is **6,656 / 9,216 bytes**, respectively, compared
with Register-RTE's **9,728 / 13,312 bytes**. These declarations and offline
resource counts establish no occupancy or speed improvement by themselves.
These selectors remain explicit experiments while the remaining qualification
and release gates are completed.

## Repaired Pair/Arena result

The repaired `direct-rte-pair` / `arena-rte` Q32 candidate uses K16 publication,
N16/stage16, with all fusion and packed publication off. Against the frozen
alpha 4 Direct-RTE/Register-RTE Q32 policy, ordinary network timings at
**1707×960**, padded to **1728×960**, are:

| Ordinary network GPU timing | Frozen alpha 4 | Repaired Pair/Arena Q32 |
| --- | ---: | ---: |
| Median | 58.684860 ms | 50.500780 ms |
| P95 | 58.922124 ms | 50.689466 ms |
| P99 | 59.053322 ms | 50.815118 ms |
| Mean | 58.662694 ms | 50.471625 ms |
| Coefficient of variation | 0.3556% | 0.3275% |

Three interleaved pairs each use five warmup and 30 measured frames: **90
samples per condition**, with image readback and dispatch instrumentation off.
Median improvement is **13.9458%**. Pooled and every paired median/P95 pass the
no-greater-than-2% regression gate, and this ordinary result exceeds the 5%
performance threshold. It does not complete the remaining promotion or release
gates. The new CLI identity begins `d6ffdde4`; its complete identities and raw
measurement hashes belong to the separate Pair/Arena scalar record.

Four repaired configurations pass **724 operators / 934 byte checks /
15,264,768 bytes each**: Init and Epilogue with Arena-RTE Q32, and Pair with
Arena-RTE Q16 and Q32. The Pair/Arena Q32 model comparison matches all **75
checkpoints and the F32 head at 320×320**. Target head and capture reproduction
are exact; this does not compare all 75 target intermediates or establish game
quality. The formal decomposed proof passes **77 checks at 320×320** and
**two checks at target resolution**. Separate profiles report actual per-frame
family medians **38.932560 → 32.151100 ms GEMM** and **7.983360 → 6.479880 ms
window attention**. Twenty-five of 46 GEMM groups and all 22 attention groups
meet the 5% operator improvement gate. These instrumented profiles remain
separate from the ordinary timing above.

The 1080p comparison uses valid 1920×1080, padded to 1920×1152, and the same
five-warmup, three-pair, 30-frame protocol.
Ordinary network median is **78.576060 → 68.383780 ms**, P95
**78.956024 → 68.760702 ms** and P99 **79.028253 → 68.861261 ms**. It supplies
external-comparison geometry timing and establishes no additional 1080p quality
or game-FPS result.

Native DLL ABI/lifecycle tests pass with eight sequential requested frames at
320×320 and the full target. The additional **source-included production-pool
harness** verifies eight distinct live prepared slots at **1707×960**, padded
to 1728×960, before submission. Serialized/prefetched output matches for eight
ordered frames (**104,878,080 bytes**) and seven output frames after a
cancel/gap/reset (**91,768,320 bytes**). Those tests retain GPU queue ordering;
they do not establish parallel inference, game performance or production-game
VRAM usage.

Bounded replay of eight existing SDR game frames passes **80 raw byte checks /
2,107,883,520 bytes**, plus **16 exact composed RGB comparisons**, in identical
and independently evolved history modes. Evolved history starts from reset and
uses each variant's own outputs; its original-capture equality flags are false
as expected. Exact inter-variant equality does not claim original ancestry for
that mode. Synthetic SDR/HDR reset, temporal and camera-reset cases also match
the prior AMD K16 route; absolute-reference highlight failures remain unresolved.

Alpha 5 includes Pair/Arena as opt-in selections. The published
alpha 4 artifacts, qualified auto cache, K16 defaults and NR-disabled setting
stay unchanged. There is no new game FPS, bridge/VRAM result, broad temporal
review, 8 ms budget or 60 FPS claim.

## Historical third-revision timing

The historical third-revision CLI uses `direct-rte-epilogue`, K16 publication,
N16/stage16 and Register-RTE Q32 attention. All fusion and packed-publication
experiments remain off. Valid input is **1707×960**, padded to **1728×960**, on
Windows 11, RX 9070 XT, Adrenalin **26.9.1 (LLPC)** and Vulkan **1.4.349**.

| Ordinary network GPU timing | Frozen alpha 4 | Historical v3 epilogue |
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
at-least-5% default improvement gate fails. The expanded partition failure
independently rejects that historical module as a preserving replacement.

The historical third-revision CLI SHA-256 is
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
| Runtime-zero initialization limited to non-residual operators with K ≤512, third revision | Both variants pass the then-current 880-check suite and recorded model/replay inputs, but fail the added K192/P96/two-batch case by 10,477 bytes | Historical proof is incomplete for the expanded contract; repair partition initialization and requalify |

The rejected N32 timing uses the full three-pair, five-warmup, 30-measured-frame
protocol: **59.189400 → 55.002080 ms median**. Its strict failures prevent preserving
promotion. Earlier short screens remain separately labeled in the evidence.

The repaired initialization path retains the original shared-memory seed for
residual operators, every partitioned operator and K >512. Its remaining
shortcut receives the positive-zero bit
pattern through a uniform push constant, rather than a compile-time matrix
constructor. Ordered K16 operations, partition reduction and publication
locations remain unchanged in source. The second revision's model failure is
retained as
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

Historical third-revision initialization-only and corrected epilogue variants
each passed **674 operators /
880 strict byte checks / 14,332,928 bytes**, with zero mismatches. Coverage includes
tails, broadcasts, residuals, partitions, activation, padding, half overflow and
subnormals, signed zeros and E4 saturation. Fourteen added ViT fixtures exercise
K1024/K4096, partitions, residuals, broadcasts and activation. They extend the
suite after the second revision passed its original operator tests but failed
the model. They do not include the subsequently failing partition regression
or qualify the repaired source.

At **320×320**, both historical variants matched all **75 model checkpoints**,
the F32 head, composition proxy and production/capture head byte for byte. At
target geometry, both matched the **head, composition proxy and production/capture head**; zero
intermediate boundaries were compared there. These use identical synthetic reset
features. The composition proxy is clamped, truncated-half display-proxy RGB,
so these comparisons do not establish scene-linear game quality. Selected
production captures are recorded separately. Fresh final-CLI epilogue and frozen
baseline runs also use the decomposed `--intermediates` route. The formal
preserving model proof passes **77 checks at 320×320**: all 75 boundaries, the
head and bundled baseline/candidate capture-production checks. Its target proof
passes **two checks**, covering head and capture-production. These synthetic
proofs apply no image-quality or performance qualification threshold.

The historical third-revision epilogue replay passed **80 byte checks /
2,107,883,520 bytes** over eight
existing genuine SDR game frames, in both identical and independently evolved
histories. It reuses the frozen alpha 4 replay reference and compares the head,
published history, runtime head and scene-linear RGBA buffers exactly. The
matched RGB frames are exact, with SSIM 1.0 and infinite PSNR against that AMD
reference across **16 frame/history comparisons**. Identical history uses the
captured ancestry; independently evolved history starts from reset and then
uses each variant's own preceding outputs. Evolved replay therefore does not
claim the original captured ancestry. This bounded replay adds no new game
capture, broad motion review or original NVIDIA parity.

The historical native runtime harness passed at **320×320 and 1707×960 with eight requested
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

The incremental K32 and final-publication screen compares against the same
AMD K16 route using unclamped scene-linear RGB and data range 1.0. Three SDR
reset/temporal/camera-reset cases pass for each policy. All three HDR-highlight
cases fail for each: the worst K32 result is **35.3022 dB / 0.985854 SSIM**, and
the worst final-publication result is **34.8592 dB / 0.985200 SSIM**. Both miss the
40 dB / 0.99 gate. This is a bounded synthetic incremental comparison and does
not replace absolute-reference or broad game quality validation. Neither
arithmetic policy is promoted.

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

Current source includes the partition repair and new named experiments; the
commands below do not reproduce the historical third-revision module bytes or
timing above. Record current identities and run the expanded suite before
assessing the result. The paired/arena delivery record provides the separate
candidate policy and its evidence.

Force the epilogue route for diagnostics. `direct-rte-init` selects the
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
