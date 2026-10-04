# Install the RX 9070 XT alpha

This guide covers the **v0.1.0-alpha.6** development package for
Cyberpunk 2077 on Windows. The existing
[alpha 5 download](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.5)
and [its installation guide](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.5/docs/INSTALL.md),
[alpha 4 download](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.4)
and [its installation guide](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.4/docs/INSTALL.md),
[alpha 3 download](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.3)
and [its installation guide](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.3/docs/INSTALL.md)
remain available unchanged for those older packages.

The opt-in C32 QKV projection/normalization route measures **50.41260 → 47.74616
ms median**, a **5.28923%** improvement with that route off versus on in the same
final production build. Both use explicit Pair/Arena K16/N16/stage16/Q32 at
1707×960 input, padded to 1728×960. Three interleaved pairs use five warmups and
30 measured frames per run, 90 retained samples per condition. This is
**network-only** GPU timing, excluding the bridge, FSR
and game; it establishes neither game FPS nor the 8 ms NR-plus-bridge target.
See the [QKV normalization delivery record](amd-qkv-normalize-delivery.md) and
[scalar evidence](performance/qkv-normalize-rx9070xt-20261003.json).
Performance and broad game-quality gates remain unmet. NR ships **disabled**,
using preserving K16 publication arithmetic and the retained qualified auto
cache. Pair/Arena and C32 QKV normalization are explicit selections; installing
alpha 6 does not activate them automatically. QKV normalization defaults to off.

**Alpha 6 game FPS has not been measured.** Historical alpha 3 benchmarks averaged
**97.12 FPS NR off / 10.69 FPS NR on**; those values belong to the
[alpha 3 game record](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.3/docs/performance/cyberpunk-alpha3-20261002.json).
Final-build replay of eight existing SDR frames passes 80 raw checks in both
history modes: 48 fresh candidate outputs and 32 copied original runtime anchors.
Sixteen computed composed outputs match the AMD Pair/Arena baseline exactly;
this adds no new scene or visual coverage. Ten-minute active gameplay, broad temporal review and
scene-linear highlight acceptance remain incomplete; existing HDR reference-quality
failures are unchanged. Complete Cyberpunk measurements remain pending.

## Requirements

- **Radeon RX 9070 XT**, Windows 11 and an AMD Vulkan driver exposing the required
  RDNA4 FP8 matrices and wave32 execution. Adrenalin **26.9.1** is the tested
  driver; other GPUs and driver versions are not validated by this release.
- An installed copy of **Cyberpunk 2077**. Version **2.31**, native DirectX 12,
  is the tested game. Start with 2560 x 1440 output, High graphics fields,
  FSR Quality, ray tracing off and frame generation off. Disable driver AFMF
  when measuring rendered game performance.
- [PowerShell 7](https://learn.microsoft.com/en-us/powershell/scripting/install/installing-powershell-on-windows)
  and the current [Microsoft Visual C++ v14 Redistributable **x64**](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
  Installing the prebuilt package does not require Visual Studio or a Vulkan SDK.
- Your own supported **nvngx_dlssnr.dll**, imported locally. NVIDIA DLLs and
  extracted model weights are **not included** in the download or repository.

## Download and check the package

1. Open the
   [v0.1.0-alpha.6 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.6)
   Download **OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip** and its **.zip.sha256** asset.
   GitHub's automatic **Source code** archives do not contain the built DLLs.
2. Compare the ZIP's SHA-256 with the sidecar, then extract it to a writable
   folder, for example `D:\OpenNR-AMD-alpha`. Keep this package for removal.
3. Open **PowerShell 7** (`pwsh`) in the extracted package's root: the folder
   containing `package-manifest.json`, `payload`, `scripts` and `tools`.

For example, in the download folder:

```powershell
Get-FileHash './OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip' -Algorithm SHA256
Get-Content './OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip.sha256'
Expand-Archive './OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip' -DestinationPath 'D:\OpenNR-AMD-alpha'
```

The installer also validates the package's managed payload hashes. Do not edit
`payload/OptiScaler.ini` or other payload files before installation.

## Install and import your model

Close Cyberpunk first. Set `$game` to the folder containing
**Cyberpunk2077.exe**, normally the game's **bin\x64** folder. The following
example uses the GOG installation path; replace it for another installation.

```powershell
$game = 'C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64'

./scripts/install.ps1 -PackageDirectory . -GameDirectory $game -WhatIf
./scripts/install.ps1 -PackageDirectory . -GameDirectory $game

./scripts/import_model.ps1 -NvidiaDll 'D:\local\nvngx_dlssnr.dll' -Destination "$game\open-nr\model"
```

Replace the `-NvidiaDll` path with your local DLL. Write permission to the game
folder is required; run that PowerShell window as administrator if the folder's
permissions deny the install or import. The installer uses `dxgi.dll` as its
default OptiScaler proxy and backs up existing managed files before replacing
them. An existing OpenNR install must be removed before installing another one.

The importer reads PE/resource bytes without loading or executing the DLL.
It accepts only these pinned containers and validates the model resource and
required tensors before publishing output:

| Supported container | Complete DLL SHA-256 |
| --- | --- |
| DLSS-NR 310.8.0 | `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` |
| DLSS-NR 310.8.SF.0 (numeric file version 310.8.2.0) | `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927` |

Both contain the same supported `WEIGHTS_HT` resource. Other versions are
rejected. The destination must be **new**: the importer does not overwrite an
existing model. If a previous installation left a verified model, reuse it;
otherwise preserve the old directory elsewhere before importing again.

After import, these paths should exist:

```text
<game>\bin\x64\open-nr\model\manifest.json
<game>\bin\x64\open-nr\model\model\stage0.bin
...
<game>\bin\x64\open-nr\model\model\stage10.bin
```

Do not copy the NVIDIA DLL into the game or share the DLL or extracted files.

## Enable and use NR

Launch Cyberpunk with FSR Quality enabled. NR is initially **off**. Press
**Insert**, open the **Neural** tab, and select **Enable NR** when ready to test.
The configured `NrBackend=mochizuki` is the host's compatibility name for loading
this package's `OpenNrRuntime.dll`; leave it as packaged.

The default INI also sets `[Spoofing] StreamlineSpoofing=false`. NVIDIA Streamline
capability spoofing is unnecessary for this FSR-based AMD integration. A startup
crash with NR disabled was traced to the unchanged host's cached Streamline
capability pointer, before the neural runtime loaded. Keep this setting disabled
in custom INI files; the host otherwise defaults it to enabled.

The OpenDLSS-NR AMD panel exposes **Effect strength**, **Colour strength**,
**Highlight guard**, **Style**, **Use motion and history**, **History strength**,
**Status**, **Render input** and **NR + bridge** timings. Defaults run one
accelerated pass at the full internal render resolution, with temporal history
enabled. The initial target input is 1707 x 960, padded to 1728 x 960.

Use **Enable NR** to turn the network off and return to ordinary FSR.
Setting effect strength to zero still runs inference. The displayed NR timing
is not game FPS. Eight final-alpha2 fixed-camera SDR frames pass both numerical
replay modes; broad scene coverage and motion/face/ghosting review remain
incomplete. Unresolved
scene-linear highlight failures are documented in
the [validation record](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.2/docs/rx9070xt-validation.md).
The [QKV normalization delivery record](amd-qkv-normalize-delivery.md) documents the current opt-in kernels,
ordinary network measurements and preservation checks. The
[alpha 3 GEMM record](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.3/docs/amd-gemm-delivery.md)
retains the prior release's evidence. The
[alpha 2 performance record](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.2/docs/amd-performance-implementation.md)
retains its historical identities.

## Optional Pair/Arena selection

Keep the normal auto selection for the existing qualified cache. To test
Pair/Arena with independent C32 QKV normalization, first close the game and start it directly from a fresh
PowerShell window with these **process-only** selections. They select kernels
without enabling NR; use Insert → Neural → Enable NR deliberately afterward.

```powershell
$game = 'C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64'
$nrPolicy = @{
  DLSS5VK_AMD_KERNELS = 'optimized'; DLSS5VK_AMD_ARITHMETIC = 'k16'
  DLSS5VK_AMD_GEMM = 'direct-rte-pair'; DLSS5VK_AMD_WINDOW_LAYOUT = 'arena-rte'
  DLSS5VK_AMD_TILE_N = '16'; DLSS5VK_AMD_STAGE_K = '16'; DLSS5VK_AMD_WINDOW_QUERIES = '32'
  DLSS5VK_AMD_FUSION = '0'; DLSS5VK_AMD_FFN32_FUSION = '0'; DLSS5VK_AMD_QKV32_FUSION = '0'
  DLSS5VK_AMD_EXPERT_FUSION = '0'; DLSS5VK_AMD_BLOCK_FUSION = '0'
  DLSS5VK_AMD_HARDWARE_PUBLICATION = '0'
  DLSS5VK_AMD_QKV_NORMALIZE = 'c32'
}
foreach ($name in $nrPolicy.Keys) {
  [Environment]::SetEnvironmentVariable($name, $nrPolicy[$name], 'Process')
}
[Environment]::SetEnvironmentVariable('DLSS5VK_AMD_TUNING', $null, 'Process')
& "$game\Cyberpunk2077.exe"
```

Use the game's executable directly: an already-running launcher may not inherit
this window's environment. Verify `direct-rte-pair`, `arena-rte`, K16/N16/stage16/Q32
and `QKV normalization c32`
in `open-nr/runtime.log`. Unsupported forced choices fail visibly and the game
host bypasses NR. These variables are read once when the session is created;
restart the game after changing them. To revert, close the game and this
PowerShell window, then launch normally. No persistent system variables or
packaged tuning files are changed. Use Enable NR to disable the effect immediately.
Change `DLSS5VK_AMD_QKV_NORMALIZE` to `off` before launching to compare Pair/Arena
without the new route. Its `c32` selection requires the exact policy above,
all other fusion/publication controls off and no tuning file. It covers only
blocks 0–4/66–70 and is independent of `DLSS5VK_AMD_QKV32_FUSION`.

## Diagnostic kernel selection and measurements

The runtime defaults to `auto` kernel selection and `k16` arithmetic. The retained alpha 4
cache contains **68 qualified GEMM/attention records** for the measured target
geometry: Direct-RTE GEMM, Register-RTE Q32 attention, N16/stage16, K16 publication
and every fusion/packed-publication experiment off. The automatic cache GPU check
selects this actual policy and reproduces the target output. Requested route
defaults remain shared/staged before a qualified auto record selects a candidate.
Auto accepts
qualified preserving kernels tied to the exact GPU, driver, model and shader
identities. A packaged `open-nr/shaders/amd-tuning.json`, when present, also binds
the qualified session geometry. Invalid or stale tuning is rejected; a qualified
record must also bind its full measured policy, including every fusion and
publication flag and the actual GEMM route and attention layout. Rejection
retains a qualified shared fallback when available.
If no qualified path fits the device's
shared-memory limit, inference is refused and the game host bypasses NR.

The diagnostic CLI exposes these controls:

| Selector | Values | Purpose |
| --- | --- | --- |
| `--amd-kernels` | `auto`, `baseline`, `optimized` | Qualified selection, explicit legacy request, or forced candidate |
| `--amd-arithmetic` | `k16`, `k32`, `final` | K16 is the alpha default; K32/final alter publication order |
| `--amd-gemm` | `shared`, `packed`, `direct`, `direct-rte`, `direct-rte-init`, `direct-rte-epilogue`, `direct-rte-pair` | Direct routes require stage K16; Pair additionally requires N16; RTE requires float controls |
| `--amd-window-layout` | `staged`, `register`, `register-rte`, `arena-rte` | Register/Arena require Q16/Q32 and FP16 accumulators; RTE layouts also require float controls |
| `--amd-qkv-normalize` | `off`, `c32` | Independent projection/normalization route; default off, c32 requires the explicit Pair/Arena policy above |
| `--amd-window-queries` | `16`, `32`, `64` | Queries per compact attention workgroup |
| `--amd-tile-n`, `--amd-stage-k` | `16`, `32`, `64` | GEMM output tile and staged K width |
| `--amd-fusion`, `--amd-expert-fusion`, `--amd-block-fusion`, `--amd-hardware-publication` | `0`, `1` | Experimental overrides; default `0` |
| `--amd-ffn32-fusion`, `--amd-qkv32-fusion` | `0`, `1` | Independent C32 route overrides; `--amd-fusion` remains shorthand for both |
| `--amd-tuning` | JSON path | Explicit qualified tuning file |

For source-built C32 operator reproduction, use
[probe_amd_c32_qkv_normalize.ps1](../scripts/probe_amd_c32_qkv_normalize.ps1).
It defaults to CPU-only `Build`. GPU `Bounded`, `Capture`, `Check` and `Timing`
modes require `-Run`, a completed `-BuildDirectory` and a fresh output path
under `build/`. `Timing` also requires the matching `Check` report. Captured
network tensors are local diagnostics and are excluded from release packages.

The corresponding runtime environment variables are `DLSS5VK_AMD_KERNELS`,
`DLSS5VK_AMD_ARITHMETIC`, `DLSS5VK_AMD_WINDOW_QUERIES`, `DLSS5VK_AMD_WINDOW_LAYOUT`, `DLSS5VK_AMD_TILE_N`,
`DLSS5VK_AMD_STAGE_K`, `DLSS5VK_AMD_GEMM`, `DLSS5VK_AMD_FUSION`,
`DLSS5VK_AMD_FFN32_FUSION`, `DLSS5VK_AMD_QKV32_FUSION`, `DLSS5VK_AMD_EXPERT_FUSION`,
`DLSS5VK_AMD_BLOCK_FUSION`, `DLSS5VK_AMD_HARDWARE_PUBLICATION` and
`DLSS5VK_AMD_TUNING`, plus `DLSS5VK_AMD_QKV_NORMALIZE`. They are read once at session creation. Scalar RTE paths
require FP16 RTE rounding, denormal preservation and signed-zero/Inf/NaN preservation
with compatible independence controls; register/arena layouts also require the enumerated
16×16 FP16 accumulator type. Pair/Epilogue additionally require F32 signed-zero/Inf/NaN
preservation. Unsupported forced choices fail visibly.
Keep the packaged defaults for game testing. Forced
diagnostic choices bypass auto qualification; `baseline` requires K16, N16/K16,
64 queries, staged attention and all overrides off. On RX 9070 XT the resource guard rejects it:
legacy attention requires 34,816 bytes and the device exposes 32,768 bytes.
The original shader and historical evidence remain retained, with no override.
Use `rte32` for new Pair/Arena comparisons, supplying the frozen alpha 4
executable and shaders explicitly. Its baseline is Direct-RTE/N16/stage16/Q32,
Register-RTE, K16 and all experiments off. The
[Pair/Arena recipe](amd-pair-arena-delivery.md#reproduce-or-opt-in) shows exact commands.
Use the explicitly labeled `direct32` anchor for RTE measurements: its baseline
runs Direct/K16/N16/stage16/Q32/staged with every fusion and packed-publication
override off. The measured comparison uses immutable alpha 3 binaries and shaders.
`qualified32` retains the earlier shared-GEMM baseline, `compact64` the earlier
legal Q64 attention baseline, and `legacy` the historical over-limit attention.
These are diagnostic-tool selections, not runtime environment variables.

From the extracted package, this collects three interleaved baseline/candidate
network pairs into a new output directory. Also extract the unchanged alpha 3
package to `$alpha3` to reproduce the published immutable-baseline comparison:

```powershell
$alpha3 = 'D:\local\OpenNR-AMD-alpha3'
./scripts/benchmark_amd.ps1 -Executable './tools/dlss5vk.exe' `
  -ShaderDirectory './payload/open-nr/shaders' -ModelDirectory "$game\open-nr\model" `
  -BaselineExecutable "$alpha3\tools\dlss5vk.exe" `
  -BaselineShaderDirectory "$alpha3\payload\open-nr\shaders" `
  -OutputDirectory './diagnostics/network-rte' -Kernels optimized -Arithmetic k16 `
  -Gemm direct-rte -WindowLayout register-rte -WindowQueries 32 -ComparisonAnchor direct32 `
  -Width 1707 -Height 960 -Warmup 5 -Frames 30 -Pairs 3
```

Python 3.10+ is required for these optional tools; NumPy is needed for SSIM.
Keep ordinary `bench` runs separate from `-Mode profile`: profiles instrument
each dispatch and report shader/specialization/shape metadata, GPU timestamp
samples and instrumentation overhead. Captures and profiles are excluded from
ordinary timing evidence. Model-only throughput must not be reported as game FPS.

`tools/qualify_amd_model.py` verifies actual binary modelcheck artifacts.
`tools/tune_amd.py` assesses paired timing records, exact suites and sequence
quality before generating tuning JSON. Synthetic model preservation does not
establish game quality or original NVIDIA parity. Follow the performance record
for building and freezing the current shaders, `amdcheck`, modelcheck,
qualification and tuning commands. New RTE comparisons pass
`--comparison-anchor direct32` to the strict qualification and tuning tools;
actual selected policies and execution identities must match throughout. The
[RTE delivery recipe](amd-rte-delivery.md) also shows how to supply the frozen
alpha 3 executable and shaders for the published comparison.

`scripts/analyze_amd_shaders.ps1 -FetchTool` optionally downloads the pinned
portable Radeon GPU Analyzer compiler using `scripts/rga_tool_manifest.json`.
It runs CPU-only wave32 ISA/resource analysis without installing a driver or
layer. Its offline compiler results are separate from installed-driver resource
statistics and GPU validation. The RGA binaries are not included in this package.

## Bounded diagnostic captures

With NR enabled, create `<game>/open-nr/capture.flag` containing a count from
**1 through 120** to request that many NR submissions. An empty file requests
one frame. The request latches once; changing its contents while it remains
present does not start another sequence. To stop or rearm, remove the flag and
allow a subsequent NR submission to observe its absence before creating it again.

```powershell
Set-Content -LiteralPath "$game\open-nr\capture.flag" -Value '8' -Encoding ascii
```

After the request completes, remove the flag before another request:

```powershell
Remove-Item -LiteralPath "$game\open-nr\capture.flag"
```

Completed frames are atomically published under
`open-nr/captures/sequence-*/frame-*`. Each includes packed source, features,
previous history, head, scene-linear output, controls and a hash manifest with
sequence/frame/submission identities, reset ancestry, exposure, jitter, model,
shader and kernel policy. Inspect `open-nr/runtime.log` for completion or errors.
Capture readbacks cost memory, disk space and GPU time; captured jobs are excluded
from the ordinary runtime timing trace. A failed allocation or retired frame can
leave the requested sequence incomplete, which replay rejects.

`tools/tune_amd.py replay --capture-sequence <sequence-directory>` can compare
identical-history and independently evolved-history runs; its default history
mode is `both`. Use the complete commands in the performance record. Capture
files remain local and are excluded from packages and repository commits.

## Remove or upgrade

Close Cyberpunk. From the package root, use the same `$game` path:

```powershell
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

Removal restores verified original managed files and preserves separately
imported models and unlisted user files. Keep the `.open-nr-install.json` ledger
and `.open-nr-backup-*` directory intact until removal completes. To upgrade,
remove the old installation using its package, then install the new one.

If **Save Settings** in the overlay changed `OptiScaler.ini`, removal will stop
before changing game files. Preserve your edited INI outside the game, restore
the packaged INI from the **same installed release**, then retry:

```powershell
$iniBackup = Join-Path (Get-Location) ('OptiScaler.user-backup-' + [Guid]::NewGuid().ToString('N') + '.ini')
Copy-Item "$game\OptiScaler.ini" $iniBackup
Copy-Item './payload/OptiScaler.ini' "$game\OptiScaler.ini" -Force
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

The generated backup filename preserves your edits in the package folder. For
any other changed managed file, preserve or move it before retrying; do not edit the ledger
or delete original backups to bypass the check.

## Troubleshooting and reports

| Symptom | Next step |
| --- | --- |
| Very low FPS with NR enabled | Clear **Enable NR**. Alpha 5 game FPS is unmeasured; historical alpha 3 NR-on averaged 10.69 FPS. The gameplay budget remains unmet. |
| Missing `MSVCP140` / `VCRUNTIME140` dependency | Install the current Microsoft Visual C++ v14 **x64** Redistributable linked above. |
| Model import rejected | Check the complete DLL hash above and choose a new destination; unsupported containers are rejected. |
| No Insert overlay | Confirm installation targeted the folder containing `Cyberpunk2077.exe`; inspect existing proxy/mod conflicts and the host log. |
| NR unavailable or bypassed | Check model paths and **Status**; run the capability query below and inspect the host log in the game folder. |
| Installer reports a changed payload | Extract an unchanged copy of the same release; do not modify the packaged payload. |
| Removal reports a changed managed file | Preserve it outside the game and follow the restoration instructions above. |

An optional capability query from the package root is:

```powershell
./tools/dlss5vk.exe info --backend amd --interop
```

Report the release tag, GPU, driver, Windows/game versions, render/output
resolution, enabled controls, status, timings and relevant error text in a
[GitHub issue](https://github.com/spydrful/OpenDLSS-NR-AMD/issues).
Keep proprietary DLLs, model files and game captures out of issue attachments.
The package includes corresponding source, build scripts and component license
notices; see its `source` directory and `NOTICE`.
