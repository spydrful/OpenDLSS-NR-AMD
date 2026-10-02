# OpenDLSS-NR-AMD

A native AMD development port of the OpenDLSS-NR neural-rendering network for
**Windows DirectX 12 games**, initially targeting the **Radeon RX 9070 XT**.
The existing Vulkan graph and model loader run through an AMD FP8 backend;
a patched OptiScaler host connects the game's D3D12 resources to Vulkan using
shared GPU buffers and fences. The AMD path requires no CUDA, PTX, DXVK or Proton.

**Status: working development implementation, with performance and game-quality
release gates still unmet.** Native inference and the D3D12 bridge have run in
Cyberpunk 2077 on an RX 9070 XT. This build is not ready for normal gameplay:
The new direct-GEMM network benchmark reduces the median from **119.143 ms to
81.049 ms**, a **31.97%** reduction against the already-qualified shared-GEMM/Q32
anchor at the target render resolution. Three warmed game benchmarks per condition
with the alpha 3 direct-GEMM runtime average **97.12 FPS with NR off / 10.69 FPS
with NR on**. The ten-minute active gameplay test requires manual game input;
broad image/temporal review remains pending.
The network timing is not game FPS or an NR-plus-bridge timing.
The goal is 60 real FPS with an initial NR-plus-bridge budget of 8 ms or less.

Current source adds packed/direct FP8 GEMM routes, independent FFN/QKV fusion
controls and isolated experiment builds. The public direct route passes strict
operator, 320×320 checkpoint and target-output comparisons and the prescribed
interleaved network timing, native lifecycle harness, bounded history replay
and automatic cache selection. Three warmed game benchmark passes per condition
are complete. See the
[GEMM continuation record](docs/amd-gemm-delivery.md). Published alpha 2 assets
and their game measurements remain unchanged.

Download the [RX 9070 XT alpha 3](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.3)
and follow the [installation guide](docs/INSTALL.md), or use the
[AMD build and setup guide](docs/AMD.md) and the
[alpha 3 game evidence](docs/performance/cyberpunk-alpha3-20261002.json).
**NVIDIA model DLLs and extracted weights are not distributed.** You supply a
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

Measured on Windows 11, RX 9070 XT, Adrenalin 26.9.1 and Cyberpunk 2077 2.31.
The full record pins model, source, shader, executable and driver identities.
The alpha 3 direct-GEMM runtime has three completed warmed NR-off and three NR-on
benchmark passes. Complete game exports, separate asynchronous runtime brackets
and eight-frame SDR numerical replay are recorded. Ten minutes of active gameplay were not run; broad
temporal/event validation remains incomplete. Earlier results are labeled below.

| Measurement | Result | Scope |
| --- | ---: | --- |
| Alpha 3 NR-off built-in benchmark, three passes | 97.32706 / 97.48801 / 96.55299 FPS | Mean 97.12269 FPS; 1440p output, FSR Quality, target High fields; frame generation and AFMF off |
| Alpha 3 NR-on built-in benchmark, three passes | **10.70176 / 10.69026 / 10.69028 FPS** | Mean **10.69410 FPS**; same settings, 972 frames per pass; one warmup per condition excluded |
| Alpha 3 pooled complete NR-off frame times | 9.980 / 13.540 / 15.290 ms | Median / P95 / P99 over 18,722 complete game-exported frame times |
| Alpha 3 pooled complete NR-on frame times | **93.495 / 95.063 / 95.979 ms** | Median / P95 / P99 over 2,916 complete game-exported frame times |
| Alpha 3 bounded runtime inference medians | 84.593 / 84.686 / 84.619 ms | Three asynchronous published-job brackets; 480 / 612 / 612 completed rows |
| Alpha 3 bounded runtime NR plus bridge medians | **85.844 / 85.948 / 85.903 ms** | Same brackets; GPU span may include waits; separate from complete game frames |
| Alpha 3 maximum observed DXGI process-local VRAM | 9,428.008 MiB | Sparse completion samples; observed maximum, not true peak |
| Final alpha 2 NR-off built-in benchmark, three passes | 96.09036 / 96.61504 / 100.27594 FPS | Mean 97.66045 FPS; 1440p output, FSR Quality, target High fields; frame generation and AFMF off |
| Final alpha 2 NR-on built-in benchmark, three passes | **7.54624 / 7.54468 / 7.54567 FPS** | Mean **7.54553 FPS**; same settings, 972 frames per pass; one warmup per condition excluded |
| Final alpha 2 pooled complete NR-off frame times | 9.88 / 13.54 / 15.34 ms | Median / P95 / P99 over 18,826 game-exported frame times; separate from PresentMon |
| Final alpha 2 pooled complete NR-on frame times | **132.55 / 133.89 / 134.637 ms** | Median / P95 / P99 over 2,916 game-exported frame times; separate from PresentMon |
| Final alpha 2 bounded runtime inference medians | 123.699 / 123.694 / 123.655 ms | Three asynchronously bounded published-job brackets; 526 / 540 / 494 rows |
| Final alpha 2 bounded runtime NR plus bridge medians | **124.953 / 124.964 / 124.952 ms** | Same brackets; GPU span may include waits; separate from complete game frames |
| Final alpha 2 maximum observed DXGI process-local VRAM | 9,607.879 MiB | Sampled every 60 completed jobs; observed maximum, not true peak |
| Earlier optimized NR-off / first NR-on benchmark | 99.58 / 99.32 / 101.74 off; 7.51 on FPS | Historical application binaries; not final release-binary measurements |
| Alpha 1 warmed built-in benchmark, three passes | 4.56 / 4.56 / 4.57 FPS | Historical NR-on configuration |
| Alpha 1 selected 600-second live-world interval | 4.5971 application FPS | Limited, mostly stationary stability session |
| Alpha 1 in-game inference median | 212.016 ms | Historical completed-job trace segment |
| Alpha 1 in-game NR plus bridge median | 213.317 ms | Same historical segment; elapsed GPU span can include handoff waits |
| Alpha 1 peak sampled process-local VRAM | 9.475 GiB | Historical game-inclusive sample |
| Historical idle GPU, model-only median | 217.949 ms | Alpha 1, 1707 x 960 valid input; not game FPS |
| Direct GEMM idle-GPU network median, qualified Q32/shared → direct | **119.143 → 81.049 ms** | Three interleaved pairs, five warmups and 30 measured frames per run; K16 arithmetic |
| Direct GEMM network P95 / P99 | 120.737 / 121.066 → 81.907 / 82.602 ms | Same paired runs; 1707 x 960 valid / 1728 x 960 padded field |
| Earlier legal Q64 → Q32 network median | 141.343 → 122.331 ms | Separate attention comparison and binary identities |
| Earlier legal Q64 → Q32 network P95 / P99 | 143.036 / 143.603 → 123.263 / 123.534 ms | Same earlier paired runs |
| Historical legacy → Q32 network median | 206.167 → 120.271 ms | Earlier binary; the legacy attention exceeds this GPU's shared-memory limit and is now rejected |

For the alpha 3 runs, in-game frame generation and driver AFMF were
observed off; the unchanged enabled driver FSR upscaling override leaves the
effective upscaler version independently unverified. Complete built-in
percentiles pool the game's rounded frame exports, and mean FPS averages the
three complete pass averages equally. No PresentMon was collected for alpha 3.
Its runtime brackets are asynchronously bounded by publication rows and are
not joined to game presents; each bracket has zero bypass-count increase.
Warmups, an earlier untraced on-screen pass and a black-screen marker are excluded.

For the historical final alpha 2 runs, in-game frame generation and driver AFMF were
observed off. Complete built-in percentiles use the game's rounded frame-time
exports, pooling actual frames rather than averaging pass percentiles; mean
FPS averages the three complete pass averages equally. Five bounded PresentMon
world subsets and three completed-runtime brackets are reported separately in
the final validation record; the second NR-off PresentMon interval is excluded
because its end was observed after the results screen. The enabled driver upscaling
override stayed unchanged, so the exact effective FSR version is not independently
established. Alpha 1 AFMF was unverified, so its historical presentation FPS
does not assert real-rendered FPS. Runtime jobs are not joined to game presents.
Network throughput and generated frames are not used as game FPS.

The [fresh final-alpha2 scalar record](docs/performance/cyberpunk-alpha2-rx9070xt-20261002.json)
pins source commit `7f1cd3108133d8aee9bae505cb31542e236d13e0` and runtime hash
`638de5aee97b65d5e091c5eb6af63df96cf38d729985cadff2b8cb79fe5c3e6c`.
Earlier game runs and the genuine eight-frame replay retain their original
identities in the [earlier scalar record](docs/performance/cyberpunk-rx9070xt-20261002.json).
The earlier legal attention comparison has its own measured binary identity in
[the legal-anchor record](docs/performance/legal-compact64-rx9070xt-20261002.json).
The [direct-GEMM record](docs/performance/direct-gemm-rx9070xt-20261002.json)
binds the new network measurements and clean native harness to their actual binaries.
The [alpha 3 game record](docs/performance/cyberpunk-alpha3-20261002.json) binds
the new complete benchmark exports and separate runtime brackets to the actual
loaded runtime and target geometry.
Final package identities are recorded separately; earlier timings are not
relabeled as fresh game benchmarks of the final binaries.

Validation completed so far:

- Direct WGSL and the portable Vulkan reference match all 75 recorded model
  boundaries and the F32 head at 320 x 320: **76 bit-exact checks**.
- The controlled D3D12 harness passes shared-buffer/fence round trips, queued
  frame ownership, cancellation/recovery, resize/reset/drain, exposure and
  continuation-state checks. Eight prefetched outputs match serialized output.
- The new public direct-GEMM comparison passes **660 operators / 862 strict byte
  checks**, 75 model checkpoints/head at 320 and target output, alongside the
  clean native eight-slot lifecycle harness and 46 qualified GEMM shape records.
- The earlier legal Q64/Q32 comparison passes **657 operators / 858 strict byte
  checks**, all 75 model checkpoints plus the F32 head at 320 x 320, and the
  full target-resolution head and composed output.
- Importer tests pass **213 checks**; the delivered source rebuilds the native
  core, shaders, runtime, importer and patched host in a fresh directory.
- Synthetic SDR compositions meet PSNR >= 40 dB and SSIM >= 0.99. Synthetic HDR
  scene-linear cases fail one or both thresholds; highlights remain unclamped.
- A preserved full-resolution frame numerically compares at **50.944 dB PSNR /
  0.999814 SSIM**, but its incorrect capture-provenance flag prevents accepting
  it as genuine game-quality evidence. It compares one frame with identical
  captured AMD history, not independently evolved reference history.
- Eight genuine frames from the earlier application binaries pass every numerical threshold
  against the portable exact reference in both identical and independently
  evolved histories: minima **49.536 dB / 0.998920** and **48.519 dB / 0.998642**.
  The short alley sequence covers camera movement and steam; broader scene
  coverage and temporal visual review remain pending.
- Eight final-alpha2 fixed-camera city frames pass every numerical threshold:
  minima **51.330 dB / 0.999792** with identical history and
  **50.525 dB / 0.999676** with independently evolved histories. Forty buffer
  checks reproduce captured production bytes exactly; the accelerated results
  still differ from the exact reference, with maximum RGB errors about 1.88/1.91.
  SDR numerical coverage does not complete broad scene or temporal visual review.

The original incorrect capture flag remains unchanged. The corrected genuine
sequence is recorded separately. Broad motion, face, exposure, cut and
disocclusion review are still pending. No original NVIDIA
runtime parity is claimed without independent original forward captures.

See [validation results and limitations](docs/rx9070xt-validation.md) for
frame-time percentiles, exact hashes, excluded smoke runs and capture scope.
Raw weights, images, captures and machine-specific logs stay local.
See the [AMD performance implementation record](docs/amd-performance-implementation.md)
for the compact attention kernels, exact preservation checks, resource reports,
qualified selection rules and reproducible commands. The earlier legal-attention
network median falls 13.45%. The earlier 41.7% reduction used the now-rejected
over-limit legacy attention and remains historical evidence. The 8 ms
NR-plus-bridge and 60 rendered FPS targets remain unmet.

## Install the alpha

1. Download **OpenNR-AMD-v0.1.0-alpha.3-rx9070xt.zip** and its **.zip.sha256** from
   the [alpha 3 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.3).
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
   alpha 3 NR-on benchmarks average 10.69 FPS. Use the checkbox to disable it; zero effect
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
`--amd-stage-k 16|32|64`, `--amd-gemm shared|packed|direct`, and `--amd-tuning <path>`.
Direct GEMM requires stage K16. Independent `--amd-ffn32-fusion 0|1` and
`--amd-qkv32-fusion 0|1` override each C32 route; the older fusion shorthand
selects both. Fusion and hardware
publication overrides default to zero. `--amd-arithmetic k32|final` changes
publication order and remains experimental; alpha auto selections use `k16`.
These choices also have `DLSS5VK_AMD_*` environment equivalents in
[amd_config.h](src/amd_config.h).

Use `scripts/benchmark_amd.ps1 -ComparisonAnchor qualified32 -Gemm direct -WindowQueries 32`
for new direct-GEMM interleaved ordinary `bench` runs and separate
`profile` runs. Profiles report per-dispatch metadata and GPU timestamps and
measure instrumentation overhead; their timings are not ordinary network or
game performance. `tools/qualify_amd_model.py` checks actual model artifacts,
and `tools/tune_amd.py` validates evidence, replays bounded sequences with
identical and independently evolved histories, and exports qualified tuning.
For direct-GEMM preservation comparisons, pass `--comparison-anchor qualified32`
to amdcheck and the collection, analysis, qualification and tuning tools. This
explicitly uses optimized shared GEMM/K16/N16/stage16/Q32 with all experiments
off as the baseline. `compact64` remains the earlier legal Q64 attention anchor;
neither selection relabels historical evidence.
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
local model) and preserves parsed output for 36 actual JSON files. It is not in
the published alpha 2 binaries; the new alpha 3 core/runtime and source rebuild
include the fix and pass native validation. See the
[implementation record](docs/amd-performance-implementation.md#source-only-json-hardening-after-alpha-2)
for the CPU regression command and distribution boundary.

## Current priorities

The [performance research notes](docs/amd-performance-research.md) collect
external kernel references and ranked experiments. Proposed changes and
author-reported external timings are separate from this fork's local results.
The [remaining performance work](docs/amd-performance-next-steps.md) separates
implementation gaps from evidence gates. The new direct instrumented profile
reports 48.796 ms FP8 GEMM and 21.033 ms window attention in an 80.805 ms
whole-frame median. Independent FFN/QKV controls are implemented;
pooling/upsampling and C512 split-FFN fusion remain partial.
The slower experimental fusion routes stay off by default.

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
