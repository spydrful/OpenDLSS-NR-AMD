# OpenDLSS-NR-AMD

A native AMD development port of the OpenDLSS-NR neural-rendering network for
**Windows DirectX 12 games**, initially targeting the **Radeon RX 9070 XT**.
The existing Vulkan graph and model loader run through an AMD FP8 backend;
a patched OptiScaler host connects the game's D3D12 resources to Vulkan using
shared GPU buffers and fences. The AMD path requires no CUDA, PTX, DXVK or Proton.

**Status: working development implementation, with performance and game-quality
release gates still unmet.** Native inference and the D3D12 bridge have run in
Cyberpunk 2077 on an RX 9070 XT. This build is not ready for normal gameplay:
The latest idle-GPU network benchmark reduces the median from **141.343 ms to
122.331 ms**, a **13.45%** reduction against the legal 64-query compact anchor
at the target render resolution. Three NR-off game benchmarks report
99.58 / 99.32 / 101.74 FPS; the first ordinary NR-on benchmark reports **7.51 FPS**.
Two further NR-on passes and the ten-minute active gameplay test remain pending.
The network timing is not game FPS or an NR-plus-bridge timing.
The goal is 60 real FPS with an initial NR-plus-bridge budget of 8 ms or less.

Download the [RX 9070 XT alpha](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.2)
and follow the [installation guide](docs/INSTALL.md), or use the
[AMD build and setup guide](docs/AMD.md) and the
[measured validation record](docs/rx9070xt-validation.md).
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
Alpha 2 has three completed NR-off benchmark passes and one ordinary NR-on
pass. Its remaining benchmark passes, active gameplay and broad temporal
quality validation are pending. Historical alpha 1 results are labeled below.

| Measurement | Result | Scope |
| --- | ---: | --- |
| Alpha 2 NR-off built-in benchmark, three passes | 99.58 / 99.32 / 101.74 FPS | 1440p output, FSR Quality, target High fields; frame generation and AFMF off |
| Alpha 2 first ordinary NR-on built-in benchmark | **7.51 FPS** | 972 frames over 129.51 seconds; two further passes pending |
| Alpha 2 NR-on bounded PresentMon sample | 133.277 / 135.669 / 136.864 ms | Median / P95 / P99; 737 intervals in a conservative 98.426-second window, distinct from the full built-in result |
| Alpha 1 warmed built-in benchmark, three passes | 4.56 / 4.56 / 4.57 FPS | Historical NR-on configuration |
| Alpha 1 selected 600-second live-world interval | 4.5971 application FPS | Limited, mostly stationary stability session |
| Alpha 1 in-game inference median | 212.016 ms | Historical completed-job trace segment |
| Alpha 1 in-game NR plus bridge median | 213.317 ms | Same historical segment; elapsed GPU span can include handoff waits |
| Alpha 1 peak sampled process-local VRAM | 9.475 GiB | Historical game-inclusive sample |
| Historical idle GPU, model-only median | 217.949 ms | Alpha 1, 1707 x 960 valid input; not game FPS |
| Current idle-GPU network median, legal Q64 → Q32 | **141.343 → 122.331 ms** | Three interleaved pairs, five warmups and 30 measured frames per run; K16 arithmetic |
| Current idle-GPU network P95 / P99, legal Q64 → Q32 | 143.036 / 143.603 → 123.263 / 123.534 ms | Same paired runs; 1728 x 960 padded field |
| Historical legacy → Q32 network median | 206.167 → 120.271 ms | Earlier binary; the legacy attention exceeds this GPU's shared-memory limit and is now rejected |

For the alpha 2 runs, in-game frame generation and driver AFMF were observed
off; PresentMon relies on that external setting verification. The bounded
NR-off pass-2 sample has median / P95 / P99 of 9.299 / 10.677 / 11.726 ms over
3,124 intervals in 28.98585 seconds. It is a subset of the 99.32 FPS built-in
pass. The NR-on subset has mean 133.231 ms; neither subset supplies full-pass
percentiles. Alpha 1 AFMF was unverified, so its historical presentation FPS
does not assert real-rendered FPS. Runtime jobs are not joined to individual
game presents. Network throughput and generated frames are not used as game FPS.

The game runs and genuine eight-frame replay used earlier application binaries,
recorded in the [scalar game evidence](docs/performance/cyberpunk-rx9070xt-20261002.json).
The current legal network comparison has its own measured binary identity in
[the legal-anchor record](docs/performance/legal-compact64-rx9070xt-20261002.json).
Final package identities are recorded separately; earlier timings are not
relabeled as fresh game benchmarks of the final binaries.

Validation completed so far:

- Direct WGSL and the portable Vulkan reference match all 75 recorded model
  boundaries and the F32 head at 320 x 320: **76 bit-exact checks**.
- The controlled D3D12 harness passes shared-buffer/fence round trips, queued
  frame ownership, cancellation/recovery, resize/reset/drain, exposure and
  continuation-state checks. Eight prefetched outputs match serialized output.
- The current legal Q64/Q32 comparison passes **657 operators / 858 strict byte
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
- Eight subsequently captured genuine game frames pass every numerical threshold
  against the portable exact reference in both identical and independently
  evolved histories: minima **49.536 dB / 0.998920** and **48.519 dB / 0.998642**.
  The short alley sequence covers camera movement and steam; broader scene
  coverage and temporal visual review remain pending.

The original incorrect capture flag remains unchanged. The corrected genuine
sequence is recorded separately. Broad motion, face, exposure, cut and
disocclusion review are still pending. No original NVIDIA
runtime parity is claimed without independent original forward captures.

See [validation results and limitations](docs/rx9070xt-validation.md) for
frame-time percentiles, exact hashes, excluded smoke runs and capture scope.
Raw weights, images, captures and machine-specific logs stay local.
See the [AMD performance implementation record](docs/amd-performance-implementation.md)
for the compact attention kernels, exact preservation checks, resource reports,
qualified selection rules and reproducible commands. The current legal-anchor
network median falls 13.45%. The earlier 41.7% reduction used the now-rejected
over-limit legacy attention and remains historical evidence. The 8 ms
NR-plus-bridge and 60 rendered FPS targets remain unmet.

## Install the alpha

1. Download **OpenNR-AMD-v0.1.0-alpha.2-rx9070xt.zip** and its **.zip.sha256** from
   the [alpha release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.2).
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
   the first ordinary NR-on benchmark measured 7.51 FPS. Use the checkbox to disable it; zero effect
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
staging, query count and fusion/publication flags; incomplete or inconsistent
policy evidence rejects that cache. If no qualified path fits the GPU's shared-memory limit, auto
refuses inference and the game host bypasses NR. The resource guard rejects an
explicit legacy baseline on RX 9070 XT: its attention requires 34,816 bytes,
above the device's 32,768-byte shared-memory limit. The original shader and
historical frozen measurements are retained; there is no limit override.
AMD selftest uses explicit `optimized` kernels because its synthetic fixtures
do not match the qualified model identity used by auto selection.

The CLI also accepts `--amd-window-queries 16|32|64`, `--amd-tile-n 16|32|64`,
`--amd-stage-k 16|32|64`, and `--amd-tuning <path>`. Fusion and hardware
publication overrides default to zero. `--amd-arithmetic k32|final` changes
publication order and remains experimental; alpha auto selections use `k16`.
These choices also have `DLSS5VK_AMD_*` environment equivalents in
[amd_config.h](src/amd_config.h).

Use `scripts/benchmark_amd.ps1 -ComparisonAnchor compact64` for interleaved ordinary `bench` runs and separate
`profile` runs. Profiles report per-dispatch metadata and GPU timestamps and
measure instrumentation overhead; their timings are not ordinary network or
game performance. `tools/qualify_amd_model.py` checks actual model artifacts,
and `tools/tune_amd.py` validates evidence, replays bounded sequences with
identical and independently evolved histories, and exports qualified tuning.
For current RX 9070 XT preservation comparisons, pass `--comparison-anchor compact64`
to amdcheck and the collection, analysis, qualification and tuning tools. This
explicitly uses optimized K16/N16/stage16/Q64 kernels with fusion and hardware
publication off as the baseline role; it does not relabel legacy evidence.
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
preserved or restored. The validation installation has been removed and the
original user settings restored.

## Current priorities

The [performance research notes](docs/amd-performance-research.md) collect
external kernel references and ranked experiments. Proposed changes and
author-reported external timings are separate from this fork's local results.

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
