# OpenDLSS-NR-AMD

A native AMD development port of the OpenDLSS-NR neural-rendering network for
**Windows DirectX 12 games**, initially targeting the **Radeon RX 9070 XT**.
The retained Vulkan graph and model loader run through AMD FP8 kernels; a
patched OptiScaler host connects D3D12 resources to Vulkan through shared GPU
buffers and fences. The AMD path requires no CUDA, PTX, DXVK or Proton.

**Status: development alpha; performance and broad game-quality gates remain
unmet. NR ships disabled.** Alpha 5 adds opt-in Pair GEMM and Arena attention,
reducing ordinary target-resolution inference from **58.685 to 50.501 ms median**,
a **13.95%** improvement against frozen alpha 4 Direct-RTE/Register-RTE.
The tested AMD output bytes match. The existing qualified auto cache stays
unchanged; Pair/Arena requires an explicit selection. These are network-only
GPU timings, excluding the bridge,
FSR and game. The **8 ms NR-plus-bridge** and **16.67 ms / 60 real FPS** targets
remain unmet.

Download the development package:
[RX 9070 XT alpha 5](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.5).
See the [alpha 5 release notes](docs/releases/v0.1.0-alpha.5.md),
[installation and optional-selection guide](docs/INSTALL.md),
[Pair/Arena delivery record](docs/amd-pair-arena-delivery.md)
and [measured evidence](docs/performance/pair-arena-rx9070xt-20261003.json).
The [verified alpha 5 publication record](docs/performance/alpha5-release-identities.json)
binds the source tag, download hashes and corresponding-source rebuild. All 33
shipped shader modules match that fresh rebuild exactly.
The earlier [alpha 4 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.4)
remains unchanged. Its [publication identities](docs/performance/alpha4-release-identities.json)
record the verified source tag, GitHub asset hashes and corresponding-source checks.
Expanded strict checks, bounded replay and native lifecycle tests pass,
including a separate full-target eight-live-slot harness. Broad game-quality
and complete-game performance gates remain unmet.
The [published alpha 3](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.3)
and [its installation guide](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.3/docs/INSTALL.md)
remain unchanged and available.

**Alpha 5 game FPS has not been measured.** Historical alpha 3 Cyberpunk benchmarks
average **97.12 FPS NR off / 10.69 FPS NR on**; those values do not describe the
new runtime. New complete game benchmarks, ten minutes of active gameplay and
broad temporal review remain pending. [Alpha 3 game evidence](docs/performance/cyberpunk-alpha3-20261002.json)
retains its original identities.

**NVIDIA model DLLs and extracted weights are not distributed.** Supply a
supported DLL locally; the importer reads it without executing it.

## Initial target

| Setting | Target |
| --- | --- |
| GPU | RX 9070 XT; sole initially validated GPU |
| Game | Cyberpunk 2077, native Windows D3D12 |
| Output | 2560 x 1440 |
| Graphics | High settings target, FSR Quality, ray tracing and frame generation off |
| NR placement | One accelerated pass at the full internal resolution, before FSR |
| Tested internal resolution | 1707 x 960, padded to 1728 x 960 for the model |
| Display | SDR; HDR display validation follows later |

The measured game uses High graphics fields but labels the edited configuration
**Custom**. This is not a claim that an unchanged High preset was verified.
Linux, additional games, ray tracing, HDR display validation and RX 7000
acceleration remain later work.

## Implemented

| Component | Current implementation |
| --- | --- |
| AMD backend | RDNA4 E4M3 FP8 cooperative-matrix kernels, wave32 and matrix/tile/memory checks, AMD weight layouts, explicit Vulkan barriers |
| Exact diagnostics | Portable numerical specification with FP16 publication, E4M3 saturation and signed zeros, fixed reductions and residual placement; independent direct WGSL diagnostics |
| Graph execution | Retained 71-block network and loader, scratch-buffer reuse, normalization/global-attention optimizations, cached pipelines |
| Native game bridge | Adapter LUID matching, D3D12-to-Vulkan shared buffers and timeline fences, GPU packing/preprocessing/inference/composition/output conversion |
| Frame lifecycle | Versioned C API, eight lazy frame slots, preparation/submission ancestry, cancellation, retirement, reset and drain; command-list binding and resource-state preservation |
| Temporal/color path | Motion reprojection, learned temporal blending, retained history precision, GPU exposure, scene-linear output before FSR |
| Model importer | Bounded PE/resource parser for explicitly pinned 310.8.0 and 310.8.SF.0 containers with identical model resources; source hash and tensor validation before publication |
| Tools and delivery | Backend/kernel-selectable diagnostics, interleaved network measurements, per-dispatch GPU profiling, qualified tuning records, offline wave32 shader analysis, bounded sequence capture, corresponding source and hash-checked install/remove scripts |

Normal frames use GPU resources and synchronization without per-frame CPU image
transfers. Diagnostic image capture is opt-in and excluded from performance
runs. Unsupported inputs or unsafe boundaries bypass NR and preserve ordinary
FSR where safe; history resets after skipped NR frames or discontinuities.

The AMD accelerated arithmetic is evaluated separately from the exact reference.
Ordinary matrix accumulation does not reproduce NVIDIA's accumulator behavior.
Successful execution is not proof of original NVIDIA parity or visual quality.
See [AMD arithmetic and optimization notes](docs/amd-numerics.md) and the
[game runtime API](game/README.md).

## Measured progress

The published alpha 4 preserving comparison uses Windows 11, RX 9070 XT and Adrenalin
26.9.1 (LLPC), valid **1707×960** input and **1728×960** padded model geometry.
Both conditions use K16 publication, N16/stage16 and Q32 attention, with fusion
and packed hardware-publication experiments off. The new route combines scalar
FP16 round-to-nearest-even publication with register attention operands and
scores. Required float controls and FP16 accumulator types are checked before
pipeline creation.

| Ordinary network GPU timing | Immutable alpha 3 Direct + staged attention | Alpha 4 Direct-RTE + Register-RTE |
| --- | ---: | ---: |
| Median | 82.224740 ms | **58.527400 ms** |
| P95 | 82.513122 ms | **58.883906 ms** |
| P99 | 82.560180 ms | **58.993574 ms** |
| Mean | 82.188681 ms | 58.500376 ms |
| Coefficient of variation | 0.296% | 0.434% |

Three interleaved baseline/candidate pairs each use five warmup frames and
30 measured frames: 90 retained samples per condition, image readback and
per-dispatch instrumentation off. The median improves **28.8202%**. Separate
instrumented profiles report actual per-frame family sums: FP8 GEMM median
**49.735 → 39.039 ms**, window attention **21.512 → 8.012 ms**. Profiles and
independently minimized dispatch spans do not replace ordinary inference timing.
The [scalar record](docs/performance/rte-kernels-rx9070xt-20261003.json) and
[raw timing records](docs/performance/rte-kernels-measurements/bench/report.json)
pin binaries, shader identities, model, driver, selected policy and scope.

The package's **68-record qualified target cache** selects Direct-RTE,
Register-RTE, K16/N16/stage16/Q32 for matching identities and geometry.
Unqualified or stale records select a qualified preserving fallback when
available; forced diagnostic choices fail visibly when capabilities are absent.
NR remains off until enabled in the overlay.

Validation of the new named routes includes:

- **660 operators / 862 strict byte checks / 14,123,008 bytes**, including tails,
  shifted windows, channel families, broadcasts, split-K, residuals, activation,
  padding and conversion edge cases.
- All **75 model checkpoints plus the F32 head at 320×320**, and production versus
  decomposed-capture reproduction. Target-resolution head reproduction passes;
  the qualified auto-selection check also passes target composition and capture.
  All 75 target boundaries were not exported.
- **36 synthetic SDR/HDR buffer checks**, byte-identical to alpha 3. This preserves
  the existing HDR-highlight failures against the exact reference; it does not
  resolve them.
- Replay of eight existing SDR game frames in both identical and independently
  evolved history modes: **80 byte checks / 2,107,883,520 bytes** match alpha 3.
  Minimum exact-reference PSNR/SSIM remain **51.330 dB / 0.999792** and
  **50.525 dB / 0.999676**, with unclamped scene-linear RGB and data range 1.0.
  This adds no new scenes or visual review.
- Controlled native shared-buffer/fence and ABI/lifecycle tests with eight
  sequential target frames. The eight-slot queued-output ownership proof runs
  at **320×320**; target-resolution eight-prefetched-output equality is not claimed.
- Installer/package/build-path checks pass **19 / 67 / 50 checks**. A fresh
  corresponding-source build passes core, runtime, importer tests and GPL host,
  reproducing all **27 native SPIR-V modules**. Identical rebuilt EXE/DLL bytes
  are not claimed.

Successful preserving comparison means equality to this fork's qualified AMD
baseline. Ordinary AMD matrix accumulation still differs from the portable
exact reference. Independent original NVIDIA forward captures are required
before any original NVIDIA parity claim.

| Historical game result | NR off | NR on | Evidence |
| --- | ---: | ---: | --- |
| Alpha 3 Cyberpunk built-in benchmark, equal mean of three warmed passes | 97.12269 FPS | 10.69410 FPS | [Alpha 3 record](docs/performance/cyberpunk-alpha3-20261002.json) |
| Alpha 2 Cyberpunk built-in benchmark, equal mean of three warmed passes | 97.66045 FPS | 7.54553 FPS | [Final alpha 2 record](docs/performance/cyberpunk-alpha2-rx9070xt-20261002.json) |

Those game results retain their original runtime, settings and frame exports.
Alpha 3 NR-on complete frame times were **93.495 / 95.063 / 95.979 ms median/P95/P99**;
its separate asynchronous NR-plus-bridge brackets were **85.844–85.948 ms median**.
The game used 1440p output, FSR Quality, target High fields labelled Custom,
with frame generation and AFMF observed off. The unchanged driver FSR upscaling
override leaves the effective upscaler version independently unverified.
Runtime jobs were not joined to game presents. These historical measurements
are not alpha 4 FPS, bridge timings or VRAM measurements.

Alpha 4 game validation is pending: Windows UI activation and recovery failed
with `GetCursorPos` access denied, before installation of the test preview.
Game settings remain unchanged. The [delivery record](docs/amd-rte-delivery.md)
tracks the remaining complete benchmark, active gameplay and visual gates.
Earlier arithmetic, WebGPU, importer and historical game evidence remains in
[AMD numerics](docs/amd-numerics.md), [RX 9070 XT validation](docs/rx9070xt-validation.md)
and the [GEMM continuation](docs/amd-gemm-delivery.md).

## Install the alpha

1. Download **OpenNR-AMD-v0.1.0-alpha.5-rx9070xt.zip**
   and its **.zip.sha256** from the
   [alpha 5 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.5).
   Check the ZIP hash, extract it to a writable folder and open **PowerShell 7**
   in the folder containing `package-manifest.json`. GitHub's automatic
   **Source code** archives do not include built DLLs.
2. Install the [Microsoft Visual C++ v14 Redistributable x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
   Use Windows 11, an RX 9070 XT and a compatible AMD Vulkan driver;
   Adrenalin 26.9.1 and Cyberpunk 2077 2.31 are the tested versions.
3. Close the game, set its **bin/x64** path and install from the extracted package:

```powershell
$game = 'C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64'
./scripts/install.ps1 -PackageDirectory . -GameDirectory $game -WhatIf
./scripts/install.ps1 -PackageDirectory . -GameDirectory $game
./scripts/import_model.ps1 -NvidiaDll 'D:\local\nvngx_dlssnr.dll' -Destination "$game\open-nr\model"
```

Replace the paths for your installation and your own supported NVIDIA model DLL.
Use a new model destination, or reuse a previously verified import. Game-folder
write permission is required; elevate that PowerShell window if needed.

4. Launch with **FSR Quality**, ray tracing and frame generation off. NR ships
   **disabled**, with K16 publication arithmetic. Press **Insert → Neural →
   Enable NR** to opt in. Performance remains far outside the gameplay budget;
   alpha 5 has no measured game FPS yet. The optional Pair/Arena selection is
   documented in the [installation guide](docs/INSTALL.md#optional-pairarena-selection).
   Historical alpha 3 NR-on
   benchmarks average 10.69 FPS. Use the checkbox to disable it; zero effect
   strength still runs NR.
5. To remove, close the game and run:

```powershell
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

Keep the extracted package and installation backups. Saving overlay settings
changes the managed INI and can stop removal; the [full installation guide](docs/INSTALL.md)
explains how to preserve it and restore the packaged file before retrying.
That guide also covers model hashes, controls, troubleshooting and upgrades.
NVIDIA DLLs, weights and game assets are excluded from release downloads.

## Build on Windows

Use **PowerShell 7**, Visual Studio 2022 C++ tools, a Windows SDK, Git, and an
AMD Vulkan driver exposing the required FP8 matrices and wave32 execution.
Windows 11 is the validated OS. Python 3.10+ is used by measurement tools;
NumPy is needed for SSIM. Node.js and a Chromium browser are optional for the
independent WebGPU model diagnostics.

From the repository root:

```powershell
./scripts/fetch_tools.ps1
./scripts/build.ps1 -Backend amd
./scripts/build_game.ps1 -SkipCore
./scripts/build_importer.ps1 -Test
./scripts/build_optiscaler.ps1 -Fetch

./build/dlss5vk.exe info --backend amd --interop
./build/dlss5vk.exe selftest --backend reference
./build/dlss5vk.exe selftest --backend amd --amd-kernels optimized
```

The capability query is not a substitute for actual shared-resource import
checks. Run the controlled bridge harness before testing a game:

```powershell
./scripts/build_interop.ps1 -Run
./scripts/test_host_safety.ps1 -RunGpu
```

The CLI accepts `--backend auto`, `amd`, `nvidia` or `reference`; use explicit
`amd`/`reference` selections for matched diagnostic runs. The original NVIDIA
backend remains available, with its separate requirements documented in the
[archived upstream guide](docs/upstream-nvidia.md).

For AMD diagnostics, `--amd-kernels baseline` explicitly requests the retained legacy implementation;
`--amd-kernels optimized` forces an explicitly selected candidate; `auto` uses
qualified K16 selections matched to the device, driver, model and shader hashes.
A local `amd-tuning.json` beside the compiled shaders can select a qualified
session geometry. Records bind the complete measured policy, including tile,
staging, query count, actual GEMM route and independent fusion/publication flags; incomplete or inconsistent
policy evidence rejects that cache. If no qualified path fits the GPU's shared-memory limit, auto
refuses inference and the game host bypasses NR. The resource guard rejects an
explicit legacy baseline on RX 9070 XT: its attention requires 34,816 bytes,
above the device's 32,768-byte shared-memory limit. The original shader and
historical frozen measurements are retained; there is no limit override.
AMD selftest uses explicit `optimized` kernels because its synthetic fixtures
do not match the qualified model identity used by auto selection.

The CLI also accepts `--amd-window-queries 16|32|64`, `--amd-tile-n 16|32|64`,
`--amd-stage-k 16|32|64`,
`--amd-gemm shared|packed|direct|direct-rte|direct-rte-init|direct-rte-epilogue|direct-rte-pair`,
`--amd-window-layout staged|register|register-rte|arena-rte`, and `--amd-tuning <path>`.
Direct GEMM routes require stage K16; the experimental Pair route also requires
N16. Register and arena attention layouts require Q16 or
Q32 and enumerated FP16 accumulator support. RTE routes additionally require
FP16 RTE rounding, denormal preservation and signed-zero/Inf/NaN float controls.
`DLSS5VK_AMD_WINDOW_LAYOUT` and `DLSS5VK_AMD_GEMM` are read once at session creation;
requested defaults stay `staged` and `shared` until a qualified auto record selects
an optimized route. Independent `--amd-ffn32-fusion 0|1` and
`--amd-qkv32-fusion 0|1` override each C32 route; the older fusion shorthand
selects both. Fusion and hardware
publication overrides default to zero. `--amd-arithmetic k32|final` changes
publication order and remains experimental; alpha auto selections use `k16`.
These choices also have `DLSS5VK_AMD_*` environment equivalents in
[amd_config.h](src/amd_config.h).

Current source builds add `direct-rte-init`, `direct-rte-epilogue` and
`direct-rte-pair` as opt-in GEMM routes, plus `arena-rte` attention.
Preserving comparisons use K16 publication, N16/stage16/Q32, with fusion and
packed hardware publication off. Epilogue and Pair additionally require F32
signed-zero/Inf/NaN preservation. The expanded suite found a partition-reset
failure in earlier experimental modules; current source retains the original
initializer for every partitioned operator and requires fresh qualification.
These routes do not change the published alpha 4 release or qualified cache.
For new comparisons, use `--comparison-anchor rte32` with explicit frozen
published alpha 4 executable and shader inputs: Direct-RTE/K16/N16/stage16/Q32,
Register-RTE, all fusion and packed publication off. See the
[post-alpha-4 experiments](docs/amd-performance-experiments.md) for the actual
variant identities, rejected candidates and qualification scope. The
[Pair/Arena continuation](docs/amd-pair-arena-delivery.md) and
[separate evidence](docs/performance/pair-arena-rx9070xt-20261003.json) track the
new repaired modules; they do not revise earlier release measurements.

Use `scripts/benchmark_amd.ps1 -ComparisonAnchor direct32 -Gemm direct-rte -WindowLayout register-rte -WindowQueries 32`
to reproduce the published alpha 4 versus alpha 3 interleaved ordinary `bench`
runs and separate `profile` runs. Profiles report per-dispatch metadata and GPU timestamps and
measure instrumentation overhead; their timings are not ordinary network or
game performance. `tools/qualify_amd_model.py` checks actual model artifacts,
and `tools/tune_amd.py` validates evidence, replays bounded sequences with
identical and independently evolved histories, and exports qualified tuning.
For RTE preservation comparisons, pass `--comparison-anchor direct32` to
amdcheck and the collection, analysis, qualification and tuning tools. This
selects Direct/K16/N16/stage16/Q32/staged with all experiments off as the baseline;
use the frozen alpha 3 binary and shaders to reproduce the published comparison.
`qualified32` remains the prior shared-GEMM comparison and `compact64` the earlier
legal Q64 attention anchor. See the [RTE delivery recipe](docs/amd-rte-delivery.md)
for exact source and runtime selections. `direct32` retains that historical
comparison meaning; use `rte32` for the newer diagnostic routes above.
The optional `scripts/analyze_amd_shaders.ps1 -FetchTool` downloads the pinned
portable RGA compiler for CPU-only wave32 resource/ISA analysis. Its results
describe that offline compiler, not the installed driver. Follow the
[performance implementation record](docs/amd-performance-implementation.md)
for the complete validation sequence.

## Import, package and install

Import a supported DLL into a **new local directory**:

```powershell
./scripts/import_model.ps1 -NvidiaDll 'D:\local\nvngx_dlssnr.dll' -Destination './models/nr'
./build/dlss5vk.exe bench --backend amd --model './models/nr' --width 1707 --height 960
```

The DLL is never loaded or executed. The strict importer checks its allowlisted
hash, resource digest and tensor layout. A matching model does not grant rights
to redistribute its source DLL or extracted bytes.

Create a local development package with the corresponding dependency source:

```powershell
./scripts/fetch_static_sources.ps1
./scripts/package.ps1 -OptiScalerDll './build/optiscaler/OptiScaler.dll' -OutputDirectory './dist/local-development'
```

Packages contain the built runtime, host, selected compiled shader inputs, diagnostic
and importer tools, dependency notices, install/remove scripts and corresponding
source. NVIDIA model DLLs, extracted weights and local game captures are excluded.
The current host build links its pinned supplied static libraries; their original
compiler configuration and byte-identical reproduction are not established.
See [static dependency sources](integrations/optiscaler/sources/README.md).

For game installation, use the folder containing `Cyberpunk2077.exe`, normally
`bin/x64`, and import the model separately into its `open-nr/model` directory.
The [AMD setup guide](docs/AMD.md#package-install-and-undo) documents install,
`-WhatIf`, configuration, controls, diagnostics and removal. The installer backs
up and hashes existing managed files; removal restores verified originals and
preserves separately imported models. Changed managed files stop removal until
preserved or restored. The alpha 3 temporary test installation has been removed
and its original user settings restored byte for byte; models were retained.
The earlier and final alpha 2 temporary test installations
have been removed and original user settings restored exactly. Imported models
and local captures were preserved. The final eight-frame diagnostic replay is
complete. Active gameplay was not run and still requires manual input; broad
quality review remains incomplete.

The JSON parser hardening passes 167 CPU checks (168 with the
local model) and preserves parsed output for 36 actual JSON files. It was added
after alpha 2 and remains included in subsequent core/runtime source. See the
[implementation record](docs/amd-performance-implementation.md#source-only-json-hardening-after-alpha-2)
for the CPU regression command and distribution boundary.

## Current priorities

The [performance research notes](docs/amd-performance-research.md) collect
external kernel references and ranked experiments. Proposed changes and
author-reported external timings are separate from this fork's local results.
The [remaining performance work](docs/amd-performance-next-steps.md) separates
implementation gaps from evidence gates. The current RTE instrumented profile
reports 39.039 ms FP8 GEMM and 8.012 ms window attention as medians of actual
per-frame family sums; ordinary full-network median is 58.527 ms. Independent
FFN/QKV controls are implemented;
pooling/upsampling and C512 split-FFN fusion remain partial.
The slower experimental fusion routes stay off by default.
The [frozen alpha 3 fusion screen](docs/performance/amd-fusion-screen-rx9070xt-20261003.json)
completes three paired protocols: FFN-only, QKV-only and both increase inference
median **23.19%, 25.57% and 50.45%**. Those original-route results are separate
from the newer RTE measurements; redesigned RTE fusion requires fresh evidence.

The historical third-revision epilogue candidate measures **58.6687 → 56.36042 ms**
ordinary target-inference median against frozen published alpha 4, a **3.934%**
improvement below the **5% default-promotion gate**. Its then-current
**674 operators / 880 strict byte checks**, recorded model output and SDR replay
passed. The expanded 934-check suite subsequently exposed **10,477 differing
bytes** in a valid K192/P96/two-batch operator in Init, Epilogue and initial Pair.
Those historical checks are insufficient for the broader contract; the repaired
modules receive new identities and fresh qualification. Global N32 remains
rejected because strict overflow outputs differ. K32/final publication also
fails the incremental synthetic HDR quality gate. The
[experiment record](docs/amd-performance-experiments.md) separates this historical
evidence from published release results; earlier releases and the qualified
auto cache/default remain unchanged.

The repaired Pair/Arena Q32 route measures **58.684860 → 50.500780 ms** ordinary
network median, a **13.9458%** improvement over frozen alpha 4 using three
interleaved pairs of 30 frames after five warmups. Readback and instrumentation
are off. Its new **724 operators / 934 strict checks / 15,264,768 bytes** pass,
as do all 75 checkpoints plus the head at 320×320 and target head/capture
reproduction. Separate profiles report actual per-frame family medians of
**38.933 → 32.151 ms GEMM** and **7.983 → 6.480 ms attention**; ordinary runs
supply the performance gate. Replay of eight existing SDR game frames passes
80 raw byte checks in both history modes. Native DLL lifecycle checks pass;
a separate source-included production-pool harness verifies eight live prepared
slots at the full target resolution, including ordered and cancel/reset paths.
Broad scene-quality review and complete game measurements remain pending. The
published alpha 4 package and qualified cache remain unchanged.

1. Improve the now-dominant FP8 matrix family, then remaining window-attention
   cost and fusion. Preserve publication boundaries and compare every change
   against matched reference inputs. Dispatch-count reduction alone is not the
   measured bottleneck.
2. Extend correctly identified game sequences and review motion, faces,
   exposure, camera cuts, disocclusion, highlights and resize behavior. Keep
   accelerated-versus-reference comparisons and original NVIDIA parity separate.
3. Resolve accelerated scene-linear precision failures and repeat full game
   benchmarks. Publish actual results against the 16.67 ms frame budget and
   8 ms NR-plus-bridge target.
4. Extend to RX 7000 and additional platforms/games after the RX 9070 XT path;
   HDR display validation and ray tracing remain later work.

## Project and licenses

The original graph, numerical specification and model implementation come from
[maanHimself/OpenDLSS-NR](https://github.com/maanHimself/OpenDLSS-NR), starting
from inspected commit `9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1` in this fork.
The host integration is adapted from the pinned
[neural-amd-opti source](https://github.com/MatheusFerreiraS/neural-amd-opti/tree/557bb8553098395f5f138c2e22ed25f256f7a3a2),
and the importer from the pinned MIT-licensed
[PE/resource extractor](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/windows/package/model-tools/dlssnr_extract_model.cpp).

The neural core retains its [MIT license](LICENSE). The importer retains its
[MIT extraction notice](tools/MODEL_IMPORTER_NOTICE.txt). The derived game
runtime and OptiScaler adapter are **GPL-3.0-or-later**, with corresponding source
and build scripts in binary packages. See [NOTICE](NOTICE) and the
[adapter license](integrations/optiscaler/LICENSE) for component notices.

This project is not affiliated with AMD or NVIDIA. Model compatibility grants
no redistribution rights to NVIDIA binaries, model weights or game assets.
The repository excludes those local assets and generated release/build output;
its checked-in arithmetic fixture is synthetic test data, not model weights.
The [documentation index](docs/README.md) links the AMD guides and original design
notes. The [upstream NVIDIA README](docs/upstream-nvidia.md) preserves the original
instructions and measurements without presenting them as AMD results.
