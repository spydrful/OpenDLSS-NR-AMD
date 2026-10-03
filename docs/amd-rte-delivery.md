# RX 9070 XT RTE kernel delivery

Alpha 4 reduces ordinary full-target network inference from
**82.224740 to 58.527400 ms median**, a **28.8202%** improvement against immutable
alpha 3 Direct/K16/N16/stage16/Q32/staged. It combines scalar FP16
round-to-nearest-even publication with a compact register attention route,
while preserving the tested AMD output bytes. The version 1 C lifecycle ABI,
Vulkan queue ordering, D3D12 resource states, fences and temporal ancestry remain
unchanged.

This measured development release is
[v0.1.0-alpha.4](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.4).
NR ships **disabled**. The **8 ms NR-plus-bridge** and **16.67 ms / 60 real FPS**
targets remain unmet. Alpha 4 complete Cyberpunk FPS, in-game bridge timings and
game VRAM have not been measured.

## Measured scope and identities

Windows 11, RX 9070 XT (`1002:7550`), Adrenalin **26.9.1 (LLPC)** and Vulkan
**1.4.349** are the qualified environment. One pass uses valid **1707×960** input,
padded to **1728×960**, before FSR. Both timing conditions retain K16 publication,
N16/stage16, Q32 attention and every fusion/packed-publication experiment off.

| Ordinary network GPU timing | Alpha 3 Direct/staged | Direct-RTE/Register-RTE |
| --- | ---: | ---: |
| Median | 82.224740 ms | 58.527400 ms |
| P95 | 82.513122 ms | 58.883906 ms |
| P99 | 82.560180 ms | 58.993574 ms |
| Mean | 82.188681 ms | 58.500376 ms |
| Standard deviation | 0.243483 ms | 0.253669 ms |
| Coefficient of variation | 0.296% | 0.434% |

The prescribed ordinary protocol completes three interleaved pairs, five warmup
frames and 30 measured frames per run: **90 retained samples per condition**.
Image readback and per-dispatch instrumentation are off. These GPU timestamps
cover the neural network; they exclude the bridge, FSR, game and presentation.
The median and P95 improvement gates pass. The result does not establish game FPS.

The [scalar evidence](performance/rte-kernels-rx9070xt-20261003.json),
[ordinary raw assessment](performance/rte-kernels-measurements/bench/report.json)
and [instrumented assessment](performance/rte-kernels-measurements/profile/report.json)
bind actual policies and input geometry to these measured identities:

| Artifact | SHA-256 |
| --- | --- |
| Immutable alpha 3 diagnostic CLI | `e93b4ac685666f63a79d2003475ace747aba36dbdce4caddf99ee143f1763c13` |
| Tested RTE diagnostic CLI | `f29be529e14a4b896b7ab4424d7618b5b6dce3aa6c8d2156f17fca6fdfca1f0f` |
| Tested native runtime DLL | `ba0a318fd5567fcf1b95cf594608ff23cb0b40b37eee0a1d2736b5b017a2a388` |
| Selected RTE shader identity | `63c5e7decfeac23900f1d98a9b1201b79a91e0dcf550079ee1cd0f9b1cd02c96` |
| Selected alpha 3 shader identity | `9497a10fc30264c5325440f90e1925f0251dc571a1676d2d1ae4aa6b810960ba` |
| Model manifest identity | `163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e` |

These identify tested artifacts. Final package, source revision and download
hashes must be recorded when packaging and publication complete; rebuilding an
EXE or DLL is not a claim of identical binary bytes. Published alpha 3 assets,
tag, [identities](performance/alpha3-release-identities.json) and game evidence
remain unchanged.

Separate profiles contain **513 dispatches per frame**, actual chronological
samples, family/shape/partition/variant/tile metadata and instrumentation overhead.
Summing each family's dispatches within each actual frame yields median
**49.735 → 39.039 ms** for FP8 GEMM and **21.512 → 8.012 ms** for window attention.
Independently minimized dispatch spans are not frame totals. Offline RGA reports
and installed-driver executable statistics are labeled separately; an RGP capture
and installed-driver ISA/matrix-instruction delivery inspection remain pending.

The [separate frozen alpha 3 fusion screen](performance/amd-fusion-screen-rx9070xt-20261003.json)
completes three full paired protocols on the original Direct/K16/Q32/staged
implementation. FFN-only, QKV-only and both increase complete-inference median
**23.1902%, 25.5734% and 50.4469%**, respectively. They remain disabled. These
results neither measure nor rule out a redesigned future RTE fusion route.

## Preserving implementation

`amd_gemm_direct_rte` uses the existing aligned direct FP8 operand loads and
ordered K16 matrix operations. Scalar `OpFConvert` replaces the expensive
software half publication at matrix accumulation boundaries. Residual scaling,
split-K reduction and activation placement retain the original helpers. The
portable exact reference and original Direct module are unchanged.

`amd_window_register` transfers four unchanged E4 bytes per word into shared
operands, directly loads typed FP16 learned priors, and retains scores in the
accumulator through the exponent epilogue before publishing typed FP16 scores.
`amd_window_register_rte` additionally uses the qualified scalar RTE helper for
matrix publication, exponent publication, prescribed softmax half additions,
reciprocal and weight publication. Both preserve physical key order, the
eight-lane reduction tree, K16 publication, exponent approximation, learned
priors, shifted-window clipping, padded zero operands and E4 saturation.

The register layout uses **9,728 bytes LDS for Q16 / 13,312 bytes for Q32**,
against **11,776 / 15,360 bytes** for the staged variants. It retains 128 threads,
four required wave32 subgroups, 64 keys and explicit phase barriers. Q64 register
layouts are rejected. Original `amd_window_small` and the retained legacy Q64
shader remain unchanged; the legacy allocation still exceeds this device's
32 KiB limit and is refused without an override.

RTE modules require FP16 **RTE rounding**, **denormal preservation** and
**signed-zero/Inf/NaN preservation**, with compatible float-control independence.
Register attention additionally requires an enumerated 16×16 FP16 accumulator
matrix type. The helper returns F32 nonfinite values before half conversion,
retaining the original helper's behavior. A same-driver conversion probe passes
**1,861,036 witnesses** against CPU/software publication; this is not exhaustive
F32 coverage. Packed hardware publication differs and stays disabled. The
[conversion/compiler evidence](performance/post-alpha3-publication-rte-evidence.json)
records explicit SPIR-V float controls, glslang **16.6.0**, RGA **2.14.2**, ISA and
source/module identities. Comment-only source corrections reproduce the tested
SPIR-V bytes; no GPU rerun is attributed to those comments.

## Selection and strict qualification

The CLI exposes `--amd-gemm shared|packed|direct|direct-rte` and
`--amd-window-layout staged|register|register-rte`; corresponding environment
variables are `DLSS5VK_AMD_GEMM` and `DLSS5VK_AMD_WINDOW_LAYOUT`, read once at
session creation. Requested defaults remain `auto`, `k16`, `shared`, `staged`.
The qualified target cache selects **Direct-RTE/Register-RTE/K16/N16/stage16/Q32**
for matching device, driver, model, shader and geometry identities.

The [68-record cache](performance/rx9070xt-26.9.1-rte-k16.json) binds complete
policy, actual module, geometry and operator evidence. Missing old layout
metadata means `staged`; it cannot qualify Register or Register-RTE. Stale or
invalid tuning retains a qualified preserving fallback when available.
Unsupported forced routes fail visibly. K32/final remain explicitly labeled,
opt-in accumulation experiments and cannot qualify preserving auto tuning.
Intermediate capture retains the selected arithmetic policy and low-level
modules; the decomposed head must reproduce production bytes.

The named K16 production route passes **660 operators / 862 strict byte checks /
14,123,008 bytes**, covering tails, shifted windows, channel families, broadcasts,
split-K, residuals, activation, padding and conversion edge cases. All **75 model
checkpoints plus the F32 head at 320×320** match the alpha 3 baseline, alongside
production/decomposed head equality. Target head reproduction passes, and the
automatic-selection check passes target composition and capture reproduction.
All 75 target-resolution boundaries were not exported. The installed Vulkan
validation layer was absent; a zero diagnostic error counter is not a claim
that validation-layer checking ran.

Existing synthetic SDR/HDR compositions pass **36 byte checks / 98,304,096 bytes**
against alpha 3. Eight existing SDR game frames pass **80 byte checks /
2,107,883,520 bytes** in both identical and independently evolved histories.
Their fixed-range-1.0, unclamped scene-linear RGB minima against the portable exact
reference remain **51.330 dB / 0.999792 SSIM** and **50.525 dB / 0.999676 SSIM**.
Reference frames are reused. This adds no new game captures or motion review and
does not fix the existing synthetic HDR-highlight quality failures. Preserving
AMD output is separate from original NVIDIA parity.

The native harness passes shared-buffer/fence ordering, imported DLL ABI and
lifecycle tests, finite private outputs and alpha preservation at 320×320 and
target geometry with **eight sequential frames**. Its separate eight-slot
ownership, cancellation/gap/reset, descriptor lifetime and prefetched/serialized
output equality checks run at **320×320**. Target-resolution eight-prefetched-output
equality is not claimed. Harness readback/validation timestamps use a different
protocol and do not substitute for ordinary inference or game measurements.

## Reproduce the comparison

Use the Windows prerequisites in [AMD.md](AMD.md#build-and-local-checks). An
isolated build keeps prior alpha binaries intact:

```powershell
$candidate = 'D:\amd\build\experiments\rte-alpha4'
./scripts/build.ps1 -Backend amd -OutputDirectory $candidate
./scripts/build_game.ps1 -SkipCore -OutputDirectory $candidate
./scripts/build_interop.ps1 -OutputDirectory $candidate
```

Import your model locally. Extract the unchanged alpha 3 package to `$alpha3`.
Use a new absent measurement directory and freeze tool/model/shader identities:

```powershell
$alpha3 = 'D:\local\OpenNR-AMD-alpha3'
$model = 'D:\local\open-nr\model'
./scripts/benchmark_amd.ps1 -Executable "$candidate\dlss5vk.exe" `
  -ShaderDirectory "$candidate\shaders" `
  -BaselineExecutable "$alpha3\tools\dlss5vk.exe" `
  -BaselineShaderDirectory "$alpha3\payload\open-nr\shaders" `
  -ModelDirectory $model -OutputDirectory 'D:\amd\build\experiments\rte-bench' `
  -ComparisonAnchor direct32 -Kernels optimized -Arithmetic k16 `
  -Gemm direct-rte -WindowLayout register-rte -TileN 16 -StageK 16 -WindowQueries 32 `
  -Width 1707 -Height 960 -Warmup 5 -Frames 30 -Pairs 3
```

Repeat with `-Mode profile` and another new output directory for instrumented
per-dispatch evidence. `direct32` names the Direct/K16/N16/stage16/Q32/staged
baseline; `qualified32` and `compact64` retain the earlier shared-GEMM and legal
Q64 comparison anchors. This operator command uses the frozen alpha 3 modules:

```powershell
& "$candidate\dlss5vk.exe" amdcheck --backend amd `
  --baseline-shaders "$alpha3\payload\open-nr\shaders" --shaders "$candidate\shaders" `
  --fixture 'D:\amd\build\experiments\rte-operators' --comparison-anchor direct32 `
  --amd-kernels optimized --amd-arithmetic k16 --amd-gemm direct-rte `
  --amd-window-layout register-rte --amd-window-queries 32 --amd-tile-n 16 --amd-stage-k 16
```

`scripts/probe_amd_publication.ps1 -RunGpu` runs the independent conversion probe;
`scripts/analyze_amd_shaders.ps1 -FetchTool` optionally fetches the pinned offline
RGA tool. Strict model qualification and tuning use the same `direct32` anchor,
actual operator/model artifacts and identity-bound timing/sequence records.
Do not run profiles or readback captures during ordinary performance collection.

## Game and release boundary

Alpha 4 game benchmarks and ten minutes of active gameplay remain pending.
Windows UI activation and recovery failed with `GetCursorPos` access denied
before installation of the new preview; game settings remain unchanged.
The historical alpha 3 **97.12 FPS NR off / 10.69 FPS NR on** belongs to its
[original game evidence](performance/cyberpunk-alpha3-20261002.json), not this
runtime. Broad motion, faces, moving objects, disocclusion, exposure and camera
cuts still require bounded captures and human flicker/ghosting review.

The development package must contain the tested native runtime and patched
OptiScaler host, selected shaders and qualified cache, importer and diagnostics,
reversible install/remove scripts, MIT notices and GPL corresponding source.
Installer, package and isolated-build path checks pass **19 / 67 / 50 checks**.
A fresh corresponding-source build passes core, runtime, importer tests and the
GPL host, reproducing all **27 native SPIR-V modules**. This is successful source
and shader correspondence, not identical rebuilt EXE/DLL bytes.
Follow [INSTALL.md](INSTALL.md). NVIDIA DLLs, extracted weights and game captures
remain excluded from commits and downloads. Prior releases stay intact. RX 7000,
other games/platforms, ray tracing and HDR display validation remain deferred.
