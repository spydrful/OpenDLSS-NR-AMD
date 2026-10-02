# OpenDLSS-NR-AMD

A native AMD development port of the OpenDLSS-NR neural-rendering network for
**Windows DirectX 12 games**, initially targeting the **Radeon RX 9070 XT**.
The existing Vulkan graph and model loader run through an AMD FP8 backend;
a patched OptiScaler host connects the game's D3D12 resources to Vulkan using
shared GPU buffers and fences. The AMD path requires no CUDA, PTX, DXVK or Proton.

**Status: working development implementation, with performance and game-quality
release gates still unmet.** Native inference and the D3D12 bridge have run in
Cyberpunk 2077 on an RX 9070 XT. This build is not ready for normal gameplay:
NR plus its bridge currently takes about 213 ms at the target render resolution.
The goal is 60 real FPS with an initial NR-plus-bridge budget of 8 ms or less.

Download the [RX 9070 XT alpha](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.1)
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
| Tools and delivery | Backend-selectable diagnostics, timing/image comparison tools, Windows build scripts, patched OptiScaler host, corresponding source, hash-checked install/remove scripts |

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

| Measurement | Result | Scope |
| --- | ---: | --- |
| Three warmed Cyberpunk built-in benchmark passes | 4.56 / 4.56 / 4.57 FPS | One NR pass before FSR at 1440p output |
| Selected 600-second live-world interval | 4.5971 application FPS | Limited, mostly stationary stability session |
| In-game inference median | 212.016 ms | Last analyzed completed-job trace segment |
| In-game NR plus bridge median | 213.317 ms | Same segment; elapsed GPU span can include handoff waits |
| Peak sampled process-local VRAM | 9.475 GiB | Includes the game; sampled every 60 completed NR jobs |
| Idle GPU, model-only median | 217.949 ms | 1707 x 960 valid input; not game FPS |

In-game frame generation was off. Driver AFMF could not be verified, so the
PresentMon results are application-presentation FPS and **real-rendered FPS is
not asserted**. The preserved completed-job trace reports zero cumulative NR
bypasses through the measured runs. Runtime jobs are not joined to individual
game presents. Network throughput and generated frames are not used as game FPS.

Validation completed so far:

- Direct WGSL and the portable Vulkan reference match all 75 recorded model
  boundaries and the F32 head at 320 x 320: **76 bit-exact checks**.
- The controlled D3D12 harness passes shared-buffer/fence round trips, queued
  frame ownership, cancellation/recovery, resize/reset/drain, exposure and
  continuation-state checks. Eight prefetched outputs match serialized output.
- Importer tests pass **213 checks**; the delivered source rebuilds the native
  core, shaders, runtime, importer and patched host in a fresh directory.
- Synthetic SDR compositions meet PSNR >= 40 dB and SSIM >= 0.99. Synthetic HDR
  scene-linear cases fail one or both thresholds; highlights remain unclamped.
- A preserved full-resolution frame numerically compares at **50.944 dB PSNR /
  0.999814 SSIM**, but its incorrect capture-provenance flag prevents accepting
  it as genuine game-quality evidence. It compares one frame with identical
  captured AMD history, not independently evolved reference history.

The original capture flag remains unchanged. A case-insensitive executable-name
fix is implemented, but corrected genuine captures and broad motion, face,
exposure, cut and disocclusion review are still pending. No original NVIDIA
runtime parity is claimed without independent original forward captures.

See [validation results and limitations](docs/rx9070xt-validation.md) for
frame-time percentiles, exact hashes, excluded smoke runs and capture scope.
Raw weights, images, captures and machine-specific logs stay local.

## Install the alpha

1. Download **OpenNR-AMD-v0.1.0-alpha.1-rx9070xt.zip** and its **.zip.sha256** from
   the [alpha release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.1).
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
   **disabled**. Press **Insert → Neural → Enable NR** to opt in. Enabled NR
   currently measured about **4.56 FPS / 213 ms NR plus bridge** at the target
   settings. Use the checkbox to disable it; zero effect strength still runs NR.
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
./build/dlss5vk.exe selftest --backend amd
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

1. Reduce full-resolution window-attention cost, then improve FP8 matrix tile
   reuse and fusion. Preserve publication boundaries and compare every change
   against matched reference inputs. Dispatch-count reduction alone is not the
   measured bottleneck.
2. Capture correctly identified game sequences and review motion, faces,
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
