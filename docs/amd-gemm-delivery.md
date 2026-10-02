# RX 9070 XT direct GEMM continuation

This continuation targets the GEMM cost remaining after the qualified Q32
attention change. It retains one full internal-resolution neural pass, valid
1707×960 and padded to 1728×960, before FSR. The published
[alpha 2](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.2)
and its measurement records remain unchanged. The public direct binary passes
strict operator/model comparisons and the prescribed ordinary interleaved timing
protocol, the controlled native runtime harness, bounded history replay and
automatic cache GPU selection. Three warmed alpha 3 game benchmarks per
condition are complete. Broad quality and active-gameplay gates remain open;
these completed checks do not establish gameplay readiness.

The [scalar evidence](performance/direct-gemm-rx9070xt-20261002.json) records
source, SPIR-V and report hashes without distributing models, NVIDIA DLLs or
game captures. The existing [performance record](amd-performance-implementation.md)
preserves the earlier legal Q64-to-Q32 comparison. The new `qualified32` anchor
uses optimized shared GEMM, K16 publication, N16/stage16 and Q32 attention,
with every fusion/publication experiment off. A GEMM improvement measured
against this anchor cannot be credited with the earlier Q32 attention gain.

## Implemented changes

[amd_gemm_packed.comp](../shaders/amd_gemm_packed.comp) packs operand transfers
while retaining shared operand staging. [amd_gemm_direct.comp](../shaders/amd_gemm_direct.comp)
loads aligned packed operands directly from GPU buffers into cooperative-matrix
fragments. The direct route removes the operand shared-memory round trip and
its barriers while retaining shared accumulator initialization and publication.
The original [shared kernel](../shaders/amd_gemm_optimized.comp) remains the
preserving fallback. The target-qualified cache selects Direct/Q32/K16
automatically for the matching model/device/driver/shader/geometry.

The direct kernel retains ordered K16 matrix operations, software half
publication after each operation, residual seeding/scaling, split-K partition
publication and reduction order, SiLU placement, and E4/half dual outputs.
It contains a uniform row-overdispatch guard before every barrier or matrix
load. A zero-initialized accumulator experiment failed **160 partition checks**
and was rejected; it is excluded from the public routes. These failures are a
separate experiment from the historical N64/stage64 regression.

The host checks aligned operand addressing and supported geometry. Direct GEMM
requires stage K16. Pipeline cache keys include the actual shader module so a
tuning remap cannot reuse another route's pipeline. Explicit Vulkan graph
barriers, frame-slot ownership, queue ordering and the version 1 C lifecycle
ABI remain in place.

The new diagnostic selector is `--amd-gemm shared|packed|direct`, or
`DLSS5VK_AMD_GEMM` in the process environment. Its default is `shared`.
The game runtime reads the same AMD process policy when creating its Vulkan
session. FFN32 and QKV32 now have independent
`--amd-ffn32-fusion 0|1` / `--amd-qkv32-fusion 0|1` and
`DLSS5VK_AMD_FFN32_FUSION` / `DLSS5VK_AMD_QKV32_FUSION` selections.
The existing `--amd-fusion` shorthand selects both routes; their separate
actual values are recorded in logs and evidence. Enabling either route is
still an experiment requiring its own GPU qualification and measurement.

Tuning, replay and model qualification bind the actual GEMM route and separate
fusion policies. Historical evidence without a GEMM field normalizes to
`shared`; it cannot certify a direct or packed route. The unchanged-variant
operator-promotion guard remains enforced. New candidates have distinct
profile variants, `amd_gemm_packed` and `amd_gemm_direct`.

## Preliminary screening

The private prototype screening used the frozen alpha 2 CLI, five warmup
frames and ten measured frames at target resolution, with image readback
disabled. It used Q32/K16/N16/stage16 with all fusion/publication experiments
off. Only the candidate shader bytes changed in an isolated shader directory.

| Screening route | Network median |
| --- | ---: |
| Qualified shared, before | 123.152 ms |
| Packed shared operands | 93.743 ms |
| Direct operands with original shared initialization | 83.399 ms |
| Qualified shared, after | 122.536 ms |

These are preliminary network-only screenings of prototype shaders. They
exclude the D3D12 bridge, FSR, presentation and game work. The public direct
shader adds the overdispatch guard and requires separate final measurements.
The screening protocol is shorter than the prescribed three interleaved
baseline/candidate pairs of 30 frames; no promotion decision uses this table.

## Public-binary ordinary target timing

The guarded public direct route completes five warmup frames followed by
three interleaved baseline/candidate pairs of 30 measured frames: **90 ordinary
frames per condition**, with image readback and dispatch instrumentation off.
Both conditions use Q32/K16/N16/stage16 with every fusion/publication experiment
off and the same final CLI, model, driver and inputs. The baseline uses the
already-qualified shared GEMM; the candidate uses the distinct direct route.

| Network statistic | Qualified shared | Public direct |
| --- | ---: | ---: |
| Median | 119.143 ms | 81.049 ms |
| P95 | 120.737 ms | 81.907 ms |
| P99 | 121.066 ms | 82.602 ms |
| Standard deviation | 0.601 ms | 0.379 ms |
| Coefficient of variation | 0.505% | 0.468% |

Median inference drops **31.97%** and P95 drops **32.16%**. The timing protocol,
whole-network regression gate and 5% default-performance gate pass. A separate
instrumented profile qualifies **46 FP8 GEMM shapes**, all improving at least 5%,
and produces 46 identity-bound direct tuning records. Automatic cache GPU
selection and bounded captured-history replay also pass. These are network
GPU timestamps, excluding D3D12 bridge, FSR,
presentation and game work. They establish neither an NR-plus-bridge budget
nor game FPS.

The instrumented profile's direct whole-frame median is 80.805 ms. Complete
per-frame family sums have medians **48.796 ms FP8 GEMM**, **21.033 ms window
attention**, **4.571 ms window normalization**, **3.183 ms F16 GEMM** and
**2.212 ms global attention**. These independently summarized family medians
do not add to the whole-frame median and are separate from ordinary timing.
FP8 GEMM remains the first measured cost, with attention the next priority.
The [raw ordinary runs](performance/direct-gemm-measurements/bench/interleaved.json),
[profile runs](performance/direct-gemm-measurements/profile/interleaved.json)
and [qualified tuning](performance/rx9070xt-26.9.1-direct-k16.json) are published
unchanged alongside the scalar record.

## Offline resource and instruction evidence

RGA 2.14.2 and its bundled LLVM 22 offline compiler analyzed the guarded public
direct SPIR-V for **32 actual GEMM specializations covering all 358 target
GEMM dispatches**. The recorded glslang invocation reproduces the input SPIR-V
byte for byte before specialization. All reports use gfx1201, 128 threads
and explicit wave32. An independent read-only audit verifies every analysis,
listed compiled-artifact hash, target dispatch index and static ISA recount.

| Compiled resource | Qualified shared | Public direct |
| --- | ---: | ---: |
| LDS per workgroup | 5,632 bytes | 4,096 bytes |
| Declared LDS | 5,376 bytes | 4,096 bytes |
| VGPR range | 22–32 | 18–28 |
| SGPR range | 27–40 | 31–46 |
| Scratch bytes | 0 | 0 |
| Reported register spills | 0 | 0 |
| Static barrier signals and waits, each | 4–10 | 2 |
| Static FP8 WMMA instructions per specialization | 1–4 | 1–4 |

Compiled LDS drops by 1,536 bytes, or 27.27%. The direct ISA contains packed
`buffer_load_b64` operand loads and removes the shared operand
`ds_store_b8` / `ds_load_b64` instructions found in the shared variant.
The matrix instruction counts remain unchanged. These are static code counts;
loops and executed frequencies are not counted. Offline compiler resources
and ISA do not establish installed-driver delivery, dynamic occupancy or a
measured latency improvement.

## Validation status

Source-only CPU checks pass: 72 configuration, 125 selection, 167 JSON-parser,
56 tuning, 37 model-qualification and 50 mocked build-path checks. The build
test parses the actual PowerShell scripts and exercises paths with spaces,
default/explicit destinations, runtime selection and environment restoration
without invoking a compiler or GPU.

The packed and direct prototypes each pass 657 operators / 858 byte checks.
The clean public-binary operator suite passes **660 operators / 862 byte checks**,
comparing **14,123,008 bytes**, including the new row-overdispatch guard checks.
The shared/direct 320×320 runs match every one of the 75 model boundaries,
the F32 head and composed RGB byte for byte. Target output matches the
**6,635,520 F32 head values** and **4,916,160 composed RGB values**. Production repeats
and production/decomposed heads match and remain finite. Independent
direct-GEMM FFN32-only and QKV32-only 320 runs each also match all 75 boundaries,
heads and composed output. Ordinary interleaved timing and the native harness
pass. The latter exercises eight-slot continuous and cancel/gap/reset ownership,
enqueue/partial-record recovery, shared GPU buffers/fences, pack/exposure,
host continuation, resize, drain and shutdown. Automatic cache GPU selection
chooses the actual Direct/Q32/K16 policy with every experiment off. An initial
public test produced passing comparison bytes but failed
shutdown because the new test destroyed a Context-owned shader module twice.
That test cleanup has been corrected; the initial report is unaccepted, and
only the clean-exit rerun qualifies the public binary.

The final CLI SHA-256 is
`e93b4ac685666f63a79d2003475ace747aba36dbdce4caddf99ee143f1763c13`;
runtime SHA-256 is
`41ca13ea3375f30b50ca6006ecf4aa95c591e5db183daa9525640b5d1552a42d`.
The actual build's `find_vcvars.ps1` selects MSVC `cl.exe` 19.44.35228.0,
SHA-256 `88c8344236a27a6e727e0a8edc49aaa2690bdc7a9464b9d18cc7abe70a9f1c0d`,
and glslang is 16.6.0. A fresh immutable delivery snapshot records those tools,
shader/source hashes and the four later build/test-source changes. Core,
runtime and shader sources are unchanged after the measured build; binary
reproduction is not claimed. The original freezes remain intact.

The development package passes **19 installer / 67 package checks** and a
fresh corresponding-source rebuild. All **24 native and two game SPIR-V files**
match the tested shader bytes. All **73 required core/runtime/shader source
files** and ten auxiliary build/test files match the current source.
The source-build proof SHA-256 is
`315de8b6fa8aa1b62802f0478d083460fd0c5dda6b19b1eb94df9a907bb5e045`.
This establishes the stated shader/source correspondence and successful rebuild,
without claiming identical rebuilt EXE/DLL bytes.

The existing eight genuine SDR game frames pass every quality threshold against
the portable reference with identical and independently evolved histories.
Minimum PSNR/SSIM is **51.330 dB / 0.999792** for identical history and
**50.525 dB / 0.999676** for evolved history. New Direct candidate output matches
the final alpha 2 Shared candidate in both modes across **80 byte checks /
2,107,883,520 bytes**, including production and decomposed heads, published
history and scene-linear output. These are bounded replays of existing captures;
they do not claim new game captures, broad motion/visual review or game FPS.

## Alpha 3 complete game measurements

The actual alpha 3 direct-GEMM runtime completes three warmed built-in benchmark
passes with NR off and three with NR on. The game is Cyberpunk 2077 2.31 at
2560×1440 windowed output, FSR3 Quality, target High fields labelled Custom,
and one 1707×960 neural pass padded to 1728×960 before FSR. Game/driver frame
generation, ray tracing and AFMF were observed off. The unchanged enabled
driver FSR upscaling override leaves the exact effective upscaler version
independently unverified.

| Complete built-in measurement | NR off | NR on |
| --- | ---: | ---: |
| Pass 1 average FPS | 97.32706 | 10.70176 |
| Pass 2 average FPS | 97.48801 | 10.69026 |
| Pass 3 average FPS | 96.55299 | 10.69028 |
| Equal-pass mean FPS | 97.12269 | 10.69410 |
| Pooled complete frame median | 9.980 ms | 93.495 ms |
| Pooled complete frame P95 | 13.540 ms | 95.0625 ms |
| Pooled complete frame P99 | 15.290 ms | 95.9785 ms |
| Complete exported frames | 18,722 | 2,916 |

The three NR-on passes each have 972 frames, taking approximately 90.83,
90.92 and 90.92 seconds. The percentiles pool actual complete game-exported
rounded frame times; they are not averaged pass percentiles or network
throughput. One warmup per condition, an earlier untraced NR-on pass and a
black-screen marker are excluded. No PresentMon was collected for alpha 3.

Separate asynchronous runtime publication brackets contain **480 / 612 / 612**
completed jobs. Their inference medians are **84.593 / 84.68616 / 84.61948 ms**
and NR-plus-bridge medians **85.84388 / 85.94782 / 85.90282 ms**. All three
brackets have zero bypass-count increase. Completion rows are not joined to
game presents, and the GPU bridge span may contain handoff waits. Sparse
process-local DXGI samples have a maximum observed **9,428.008 MiB**; this is
not a true interval peak. The [alpha 3 scalar game record](performance/cyberpunk-alpha3-20261002.json)
binds the actual loaded runtime, shader/tuning hashes, exports and bracket
boundaries without including game assets or images.

Pooling the **1,704 selected completed jobs** gives inference
**84.63244 / 85.80519 / 86.727676 ms** and NR-plus-bridge
**85.90128 / 87.105116 / 88.0618204 ms median/P95/P99**. These completed-job
statistics stay separate from the complete built-in frame distribution.
The temporary test package has been removed, original user settings restored
byte for byte, and imported models retained locally.

The [final alpha 2 validation](cyberpunk-alpha2-validation.md) retains its
historical **97.66 FPS off / 7.55 FPS on** and approximately **124.95–124.96 ms**
bounded runtime medians unchanged. The new game results are measured separately.
The **8 ms NR-plus-bridge** and **16.67 ms complete-frame / 60 FPS** targets
remain unmet. Broad quality and the ten-minute active-gameplay test remain open,
and NR remains disabled by default.

## Reproducible isolated builds

The core, shader, runtime and harness build scripts accept `-OutputDirectory`.
Omitting it retains the existing `build` destinations. A caller-selected path
is resolved absolutely and forwarded to dependent scripts. These commands
build into a separate directory and leave existing alpha binaries in place:

```powershell
$experiment = 'D:\amd\build\experiments\direct-gemm'
pwsh -NoProfile -File scripts/build.ps1 -Backend amd -OutputDirectory $experiment
pwsh -NoProfile -File scripts/build_game.ps1 -SkipCore -OutputDirectory $experiment
pwsh -NoProfile -File scripts/build_interop.ps1 -OutputDirectory $experiment
```

The output contains `dlss5vk.exe`, `shaders`, `game\OpenNrRuntime.dll`,
`game\shaders` and `interop\interop_selftest.exe`. To run the controlled native
harness, supply a local assets root containing `model\manifest.json` and
the matching runtime `shaders` directory:

```powershell
pwsh -NoProfile -File scripts/build_interop.ps1 -SkipCore `
  -OutputDirectory $experiment -RuntimeAssets 'D:\local\open-nr' `
  -Width 320 -Height 320 -Frames 8 -Run
```

The harness covers shared-buffer/fence round trips and its separate eight-slot
ownership checks, cancellation, resize/reset, drain and shutdown. `-Frames 8`
selects the ordinary frame-loop length; the eight-slot ownership test is a
separate test. `-RuntimeDll` optionally overrides the isolated DLL location.
No command above installs into Cyberpunk.

Collect ordinary timing separately from instrumented profiling, using a fresh
absent output directory and a locally imported model:

```powershell
python tools/tune_amd.py collect --executable "$experiment\dlss5vk.exe" `
  --model 'D:\local\open-nr\model' --shaders "$experiment\shaders" `
  --output 'D:\amd\build\experiments\direct-gemm-bench' --mode bench `
  --comparison-anchor qualified32 --gemm direct --kernels optimized `
  --arithmetic k16 --tile-n 16 --stage-k 16 --window-queries 32 `
  --width 1707 --height 960 --warmup 5 --frames 30 --pairs 3
```

Use `--mode profile` and another new output directory for per-dispatch evidence.
The tools validate matched selected policies and identities; forcing a route
cannot silently produce shared-route evidence. Final acceptance additionally
requires actual strict output artifacts, target production/decomposed output,
identical/evolved history replay, the operator/whole-network improvement gates,
and renewed game validation before a release can claim game performance.

This continuation distributes source and scalar evidence. NVIDIA DLLs,
extracted weights and game captures remain local. Core MIT attribution and the
GPL adapter's corresponding-source/license requirements remain unchanged.
