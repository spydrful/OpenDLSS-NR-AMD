# Native AMD development build

This fork targets Windows x64 and the Radeon RX 9070 XT first. A patched
OptiScaler host intercepts the game's Direct3D 12 upscaler input. `OpenNrRuntime.dll`
runs this repository's neural graph in Vulkan on the same adapter, then supplies
a private Direct3D 12 texture to FSR. Shared GPU buffers and fences connect the
APIs; DXVK is not required. RX 7000 support is a later target.

The first game target is Cyberpunk 2077 at **2560×1440 display output, with NR
before FSR at the game's lower render resolution**. Start with one NR pass,
FSR Quality, ray tracing and ray reconstruction off, and frame generation off.
The goal is 60 real rendered frames per second, including the game, NR, and
FSR. It is a goal, not a measured result. Running NR at native 2560×1440 instead
processes more pixels and needs a separate performance budget.
The current idle-GPU model-only comparison measures **141.343 → 122.331 ms
median**, a **13.45%** reduction from legal Q64 compact attention to Q32,
at valid 1707×960, padded to 1728×960, before bridge, game or upscaler work.
This remains above the 8 ms NR-plus-bridge and 60 FPS budgets. The historical
206.167 → 120.271 ms comparison used legacy attention exceeding this GPU's LDS
limit; the current runtime rejects that path. The alpha 1 median was 217.949 ms.
See the [performance implementation record](amd-performance-implementation.md)
for the current configuration, hashes and measurement scope.

Three alpha 2 NR-off Cyberpunk built-in benchmarks report 99.58, 99.32 and
101.74 average FPS. The first ordinary NR-on pass reports **7.51 FPS**, with
972 frames over 129.51 seconds; two additional passes and ten minutes of active
gameplay remain pending. In-game frame generation and driver AFMF were observed
off for these runs. Conservative PresentMon subsets are reported separately
from full built-in benchmark results in the performance record.
The game runs and eight-frame genuine replay used earlier application binaries;
the current legal network measurement and final package identities are recorded
separately. The shader aggregates are unchanged, but these are not fresh game
benchmarks of the final binaries.

The older alpha 1 NR-on passes reported 4.56, 4.56 and 4.57 FPS, with about
213 ms NR-plus-bridge time. Driver AFMF was unverified in that historical test,
so those older presentation FPS values do not assert real-rendered FPS.
The [validation record](rx9070xt-validation.md) includes the actual settings,
frame-time percentiles, VRAM samples, numerical comparisons and remaining
game-quality checks. The temporary alpha 2 test installation was removed and
the original user settings restored byte for byte; imported models and local
captures were preserved.

## Validation status

Treat this as a development build. Compilation, model-free operator tests,
synthetic bridge tests, and install/uninstall tests do not establish complete
model parity or Cyberpunk compatibility. In particular:

| Gate | Evidence required |
| --- | --- |
| Operator arithmetic | Portable reference operators versus the CPU oracle, including FP8 special values and half publication |
| Full model reference | Matched WebGPU or established NVIDIA captures with all declared boundary/head/output comparisons passing |
| AMD optimized quality | The same inputs, model bytes, random seed, controls, and history; image and temporal differences recorded against reference |
| Direct3D 12 game bridge | Real producer/consumer ordering, matching adapter LUID, GPU shared-memory/fence import, input state restoration, motion and reset tests |
| Game quality | Static and moving Cyberpunk sequences, camera cuts, exposure changes, disocclusion, HDR, and resolution changes |
| Performance | End-to-end frame times and GPU stage times from the real game, warmup excluded, frame generation off |

The direct WebGPU diagnostic passes all 75 recorded boundary buffers and the
head against the local Vulkan software reference with the actual imported
model at 320×320. The original optimized browser path still differs on this
Radeon; `validate_model.ps1 -OptimizedWebGpu` reproduces that separate result.
The accelerated AMD schedule also differs from the software reference.
See [amd-numerics.md](amd-numerics.md) for the specific inputs, arithmetic and
measured differences. These checks do not establish NVIDIA-runtime parity,
captured-game temporal/image quality or HDR suitability. The real-game quality
and 60 FPS release gates remain unmet. The upstream README's NVIDIA benchmark
and parity claims describe upstream work, rather than measurements of this AMD
fork. No model weights or reference captures are distributed here.

The legal Q64/Q32 preserving comparison passes 657 operators / 858 strict byte
checks, all 75 model checkpoints and the F32 head at 320×320, and complete
target-resolution output. Eight genuine target-resolution game frames pass
the composed RGB thresholds against the portable exact reference with both
identical and independently evolved histories. Their short alley sequence and
limited spatial inspection leave broader scene coverage and temporal review
pending; they do not resolve the synthetic HDR-highlight failures below.

The initial game release evaluation targets an SDR display. HDR display
validation is deferred by the user; the separate HDR synthetic diagnostics
below remain recorded evidence rather than a prerequisite for the initial SDR
release. Motion, cuts, exposure, highlights, disocclusion, resize and reversible
color-state checks still apply to the initial game quality gate.

The generated HDR scene diagnostic also fails the agreed composed-image
thresholds at a fixed linear RGB data range of 1. These are synthetic
pre-FSR compositions, rather than captured-game or NVIDIA comparisons:

| Generated HDR case | PSNR | SSIM | PSNR ≥ 40 / SSIM ≥ 0.99 |
| --- | --- | --- | --- |
| Reset | 32.60 dB | 0.99071 | Fail / pass |
| Temporal history | 35.70 dB | 0.98610 | Fail / fail |

These remain unresolved composed-image quality failures. The same numerical
thresholds apply to scene-linear highlights where they occur in the initial
SDR target; an SDR display does not remove that quality requirement. A passing
boundary diagnostic or successful game load does not approve these errors.
HDR display validation is deferred and is not an additional prerequisite for
the initial SDR evaluation.

The bridge has versioned metadata for render rectangles, jitter, pre-exposure,
and an exposure texture. A correct result depends on the host providing the
game's actual values at the pre-FSR scene-color point. Tone-mapped screenshots
alone cannot validate scene-linear composition. Missing motion requires a
history reset or a spatial-only frame; invented zero motion is not evidence
of temporal correctness. Depth-based disocclusion and complete game/HDR
validation must be evaluated separately from the network's output quality.

The normal bridge path uses GPU queue waits and fences. CPU waits are confined
to teardown and failure recovery. The host publishes job ownership before
recording any commands, and retains a failed recording until its command list
is discarded or its actual submitted queue completes. Failed enqueue uses the
optional `OpenNrRecoverSubmission` export to establish original-color fallback
before the continuation. Failed recovery suppresses the unsafe continuation
and retains the job, session and runtime DLL; it requires a graphics restart.
Successful recovery retires the consumer, then drains and recreates the
session. Toggling NR through the menu or hotkey invalidates temporal history.
The focused CPU ownership checks, real D3D12 continuation-copy tests, and
native recovery/binding harness pass on the RX 9070 XT. These safety checks do
not satisfy the image-quality or performance gates.

## Build and local checks

Use Windows x64, an up-to-date AMD Vulkan driver, Visual Studio 2022 C++ tools,
and a Windows SDK. Use PowerShell 7 for the build and packaging scripts.
From the repository root:

```powershell
./scripts/fetch_tools.ps1
./scripts/build.ps1 -Backend amd
./scripts/build_game.ps1 -SkipCore
./scripts/build_importer.ps1 -Test
./tests/install_tests.ps1
./tests/package_tests.ps1
./scripts/test_host_safety.ps1 -RunGpu
python ./tests/test_measurement_tools.py
./build/dlss5vk.exe info --backend amd --interop
./build/dlss5vk.exe selftest --backend reference
./build/dlss5vk.exe selftest --backend amd --amd-kernels optimized
```

Python 3.10 or later runs the timing helper. Image SSIM requires NumPy;
install it in your Python environment with `python -m pip install numpy` if
it is absent. PPM/PFM parsing and MAE/PSNR need only Python's standard library.
If `python` is absent from PATH, use the full path to an installed Python.
The selftests use synthetic tensors and CPU arithmetic oracles, not imported
NVIDIA weights. The importer tests exercise malformed PE resources, truncated
records, strict source hashes, raw-byte preservation, stage ranges, and refusal
to overwrite a destination. Installer tests use a temporary synthetic game
folder, never a real game install.

AMD selftest explicitly selects `optimized`: its synthetic fixtures have no
qualified model identity for auto selection. The portable reference selftest
is unchanged. `--amd-kernels baseline` still requests the original legacy
kernel, but the shared-memory guard rejects it on RX 9070 XT because its
34,816-byte attention allocation exceeds the 32,768-byte device limit. The
original shader and historical frozen measurements are retained without an
override. For new strict comparisons and paired timings, use the explicit
`--comparison-anchor compact64` tool option (or
`benchmark_amd.ps1 -ComparisonAnchor compact64`). Its baseline role is
optimized K16/N16/stage16/Q64, with all fusion and hardware-publication flags
off. See the [reproduction recipe](amd-performance-implementation.md#reproducing-qualification)
for the full current-shader snapshot and identity-bound qualification.

The core exposes `--backend reference`, `--backend amd`, and `--backend nvidia`.
Reference is intended for numerical verification; AMD selects the optimized
path subject to actual device capabilities. An AMD kernel's successful
compilation alone does not prove whole-model equivalence. Inspect the capability
report and retain it with each benchmark. Extension presence alone does not
prove that a Direct3D 12 resource or fence handle can be imported successfully.

Build the pinned host with this fork's `fetch_optiscaler.ps1` and
`build_optiscaler.ps1` scripts; use the patched host binary,
not an unrelated OptiScaler release. The adapter pin is
`MatheusFerreiraS/neural-amd-opti@557bb8553098395f5f138c2e22ed25f256f7a3a2`.
The v1 configuration selector remains `NrBackend=mochizuki` for compatibility;
the patched host loads **OpenNrRuntime.dll**.

## Import your locally supplied model

The importer accepts only these two explicitly pinned `nvngx_dlssnr.dll`
containers, both with the exact same pinned `WEIGHTS_HT` resource:

```text
310.8.0 original:
  e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e
310.8.SF.0 supplied container (fixed numeric version 310.8.2.0):
  6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927
```

It does not download that DLL. Supply a copy you are entitled to use. A hash
match identifies the supported bytes; it does not grant redistribution rights.
The importer is a bounded PE/raw-record parser derived from the MIT extractor
at `mochizuki0323/DLSSNR-AMD@82560c4fbfaac347fc5e22c22025191402ae916b`.
Its notice is in `tools/MODEL_IMPORTER_NOTICE.txt`.

```powershell
./scripts/import_model.ps1 -NvidiaDll 'D:\local\nvngx_dlssnr.dll' -Destination 'D:\models\nr-310.8.0'
```

The destination must not exist. On success it contains `manifest.json` and
`model/stage0.bin` through `model/stage10.bin`: all 153 raw tensor records over
71 blocks, preserved without deswizzling or converting FP8 values. The manifest
matches this fork's original model loader. The other project's `NRMODEL1` file
format and unpacked 599-entry bundle are not interchangeable with this format.
Invalid or unpinned input does not publish a model directory. Stage hashes are
checked by the runtime when it loads the model.

To inspect a different DLL container without executing it or extracting weights:

```powershell
./build/importer/model_importer.exe --inspect 'D:\local\nvngx_dlssnr.dll'
```

This emits JSON containing the DLL hash/version number, the actual PE resource
offset/length/hash, record counts, per-tensor hashes/dimensions, and proposed
native-stage hashes. It does not write model files or bypass the import pin.
The original author's [pinned public metadata](https://huggingface.co/inarikami/dlss5-nr-reverse-engineering/blob/2f3db2562d18f750169c85ed947dc15bffca5a3b/docs/nr-model-access.md)
pins `WEIGHTS_HT` to 147,695,410 bytes with SHA-256
`836f445d06ecd2e59bb9f17b84b91c143396fd76ccda1c9dc7fe81d5edd548f4`.
The locally supplied `310.8.SF.0` container, DLL SHA-256
`6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`, has
fixed numeric version 310.8.2.0 and moves that resource to file offset 18,128,056.
Its resource matches that original digest exactly: the embedded model bytes
are identical even though the DLL container differs. This finding is about
model bytes and does not establish game image quality or performance.
The importer requires both an allowlisted DLL hash and that exact resource
length/hash; a matching resource inside an arbitrary DLL is insufficient.

For a game install, import directly into a new `<game executable folder>/open-nr/model`
directory, or copy the complete imported directory there. Thus the game layout
is `open-nr/model/manifest.json` and `open-nr/model/model/stage0.bin`, and so on.
Keep models out of release packages and source control.

## Package, install, and undo

Package the built runtime and patched OptiScaler DLL into a new output folder:

```powershell
./scripts/fetch_static_sources.ps1
./scripts/package.ps1 -OptiScalerDll './build/optiscaler/OptiScaler.dll' -OutputDirectory 'D:\amd\dist\local-test'
```

The source fetcher retrieves the five hash-pinned static-dependency archives
needed for the corresponding-source package. It verifies existing archives
and refuses differing files. See `integrations/optiscaler/sources/README.md`
in the corresponding-source tree.

The script includes `build/shaders/*.spv`, `build/game/shaders/*.spv`, and
`build/game/shaders/bridge.hlsl`, plus the pinned SDK's AMD FidelityFX DX12
loader/upscaler under `OptiScaler/` and DirectX Agility DLLs under
`OptiScaler/D3D12_OptiScaler/`, with their license/notices. `-RuntimeDll`, `-ShaderDirectory`,
`-ShaderFileNames` selects an exact basename allowlist to exclude unapproved
experimental binaries. `-GameShaderDirectory`, `-BridgeShader`, `-OptiScalerSource`, `-FidelityFxDirectory`,
`-AgilityDirectory`, `-DiagnosticExe`, and `-Configuration`
can select explicit inputs. The manifest records SHA-256 hashes for every
installed file and explicitly records unmet whole-model, composed-image,
game temporal/highlight and end-to-end performance release gates. HDR display
validation remains deferred. A package has an
importer, diagnostic scripts and corresponding source alongside
the payload; it does not contain NVIDIA DLLs or weights. The source snapshot
preserves tracked SDK linker libraries and places the modified host at
`source/OpenDLSS-NR-AMD/third_party/optiscaler-host/`, matching build-script
paths. Successful source compilation and real FSR initialization are separate
checks; packaging alone does not establish that the game can load FSR.
The browser WebGPU reference/diagnostic source is included under
`source/OpenDLSS-NR-AMD/ports/browser-webgpu/`; generated fixtures, model files,
node_modules and generated dist directories are excluded.
`tests/source_package_build.ps1 -PackageDirectory <package>` rebuilds a scratch
copy of the bundled source with the local MSVC/Windows SDK and cached glslang,
leaving the package unchanged. It builds native shaders/core, runtime, importer,
and host; it does not launch the game or import a model.

The package's `tools/dlss5vk.exe` is a diagnostic CLI and stays outside the
game install. Its hash is recorded separately from installed payload files.
Both core and game SPIR-V files are in `payload/open-nr/shaders/`; pass that
directory explicitly for both shader arguments when replaying a capture:

```powershell
./tools/dlss5vk.exe compositecheck --backend amd --model '<local imported model>' --recorded-frame '<local capture folder>' --fixture '<new replay folder>' --shaders './payload/open-nr/shaders' --game-shaders './payload/open-nr/shaders'
```

Use `--backend reference` with the same recorded inputs for a portable replay.
Replay and model-only diagnostics do not measure game FPS or approve image
quality. Imported weights, generated captures and replay files stay outside
the package.

Close the game, select the folder containing its executable, and inspect the
planned change first. For Cyberpunk that is normally its `bin/x64` folder:

```powershell
./scripts/install.ps1 -PackageDirectory 'D:\amd\dist\local-test' -GameDirectory 'D:\Games\Cyberpunk 2077\bin\x64' -WhatIf
./scripts/install.ps1 -PackageDirectory 'D:\amd\dist\local-test' -GameDirectory 'D:\Games\Cyberpunk 2077\bin\x64'
```

These commands are examples; use the actual local game path. The default
proxy name is `dxgi.dll`; `-ProxyName winmm.dll` or `version.dll` selects a
different host loader name. Existing managed files, including an existing
proxy or `OptiScaler.ini`, are copied into a unique `.open-nr-backup-*` folder
and hash-verified before replacement. `.open-nr-install.json` records each
target, original hash, installed hash, and backup. Interrupted installs retain
their recovery ledger. A second installation refuses an existing ledger.

After installation, import the model separately. Start with the packaged
single-pass pre-FSR configuration. If the model is absent, the adapter must
report an unavailable runtime and preserve the game's normal upscaler path;
that behavior requires a real host/game test before a release claim.

```powershell
./scripts/uninstall.ps1 -GameDirectory 'D:\Games\Cyberpunk 2077\bin\x64' -WhatIf
./scripts/uninstall.ps1 -GameDirectory 'D:\Games\Cyberpunk 2077\bin\x64'
```

Uninstall verifies all targets before changing any, restores verified originals,
and removes unchanged newly installed files. If the game or user has modified
a managed file, including a saved `OptiScaler.ini`, uninstall stops and keeps
the file, ledger, and backups. Move that edited file to a safe location or
restore the installed/original version, then retry. Imported models, unrelated
files, and unlisted backup files remain. Empty directories may remain. Do not
delete the ledger or backups until recovery is complete.

## Record quality and performance

Collect paired runs on the same save, camera path, controls, random seed, render
resolution, exposure, and history sequence. Compare NR off, portable reference,
and AMD optimized paths with the same FSR settings. Include still scenes,
rapid pans, thin geometry, faces, dark/highlight transitions, and camera cuts.
Compare both pre-FSR scene-color and final 1440p output when capture tooling
supports them. Capture HDR values before display tone mapping as float PFM;
record the units and fixed PSNR peak. SDR PPM comparisons are normalized to
their declared maximum.

```powershell
python ./tools/compare_images.py reference.pfm amd.pfm --peak 1 --output comparison.json
python ./tools/compare_images.py --sequence sequence.json --min-psnr 40 --min-ssim 0.99 --output quality-gate.json
python ./tools/analyze_performance.py game-timings.csv --warmup 120 --output performance.json
```

The image helper reports MAE, RMSE, maximum error, PSNR, SSIM, and exact equality.
It accepts matched P6 PPM or PF/Pf PFM captures; it does not align frames,
estimate optical flow, or independently detect temporal artifacts. Exact equality is
reported with `psnr_db: null` because PSNR is infinite. Optional `--min-psnr`
and `--max-mae` thresholds and `--min-ssim` return a failing exit code when unmet.
The agreed optimized-output gate is **PSNR ≥ 40 dB and SSIM ≥ 0.99 for every
matched composed RGB frame**, alongside visual and temporal review. SSIM uses
an 11×11 Gaussian window with sigma 1.5, population moments, K1=0.01 and K2=0.03,
and the explicit `peak` as data range. It averages valid-window scores within
each channel, then averages the channels. Window centers within five pixels
of an edge are excluded; no edge padding is used. No color-space conversion
or gamma adjustment is applied by the tool. At least 11×11 captures and NumPy
are required for SSIM. A single still-image score cannot approve temporal artifacts.

Threshold gates require `--metadata paired-capture.json` or a sequence manifest.
The sequence format is `OpenNR-quality-sequence-v1`, with a positive
`data_range` and a nonempty `frames` array. Each frame has `reference` and
`candidate` image paths relative to the manifest and a `metadata` object with
separate `reference` and `candidate` objects. Both objects must match on:

```text
frame_id, sequence_id, seed, model_sha256, input_sha256, controls,
render_resolution, output_resolution, pipeline_point, color_space,
pre_exposure, exposure_scale, jitter, motion_scale, reset, history_frame_ids
```

Hashes are lowercase SHA-256 strings. Resolutions and motion/jitter are two-item
arrays; `controls` is a nonempty object; reset is boolean. `pipeline_point`
should be `pre-fsr` for composed render-resolution RGB or `post-fsr` for output
RGB. Histories list unique ordered earlier frame IDs and must be empty on reset.
Frame IDs must increase within a sequence; a gap requires a reset. The helper
checks matching metadata and dimensions, but capture tooling must truthfully
record those fields and hash the original input. It cannot reconstruct motion
or verify that declared metadata reflects a game's GPU resources.

The timing CSV requires `frame_ms`, the measured interval for **real rendered
frames with frame generation off**. Optional `neural_ms`, `pack_ms`,
`inference_ms`, and `unpack_ms` columns need GPU timestamps covering that work.
Optional `vram_mib` records actual GPU memory usage; `bypassed` is 0 or 1 per
frame, with an optional `bypass_reason`. The report includes peak/mean VRAM,
bypass counts/reasons, and whether all measured frames executed NR. A timing
run containing bypasses cannot pass an NR performance gate. Runtime allocated
neural bytes cover only part of the working set and must not be reported as
total VRAM; use DXGI or an external GPU memory capture for `vram_mib`.
CPU enqueue time is not GPU inference time. The helper excludes the chosen
warmup samples and reports mean, median, p95, and p99. `1000 / mean(frame_ms)`
is end-to-end rendered FPS; `1000 / inference_ms` is not game FPS. Its p95
16.667 ms budget indicator is useful evidence for the 60 FPS target, but does
not replace run-to-run stability, memory usage, or quality checks. The core's
synthetic `bench` and per-dispatch-minimum `profile` tools characterize model
work, not complete game performance.

For end-to-end capture, `scripts/fetch_presentmon.ps1` fetches the official
[PresentMon 2.3.1 console release](https://github.com/GameTechDev/PresentMon/releases/tag/v2.3.1)
and checks SHA-256
`364e5d98d4d134bd54dd25c22ed2ca2f4883f8bc3ed6502bee0c151e3436d30c`.
It does not launch PresentMon. Keep application and driver frame generation off
and capture one reproducible benchmark run per file, with the same save,
settings, render/output resolutions, camera path and warmup for NR off/on.
Use the game's actual PID if multiple instances share the executable name.
The [pinned console documentation](https://github.com/GameTechDev/PresentMon/blob/v2.3.1/README-ConsoleApplication.md)
defines the capture flags; the example retains all presents:

```powershell
./scripts/fetch_presentmon.ps1
./tools/presentmon/PresentMon-2.3.1-x64.exe --process_name Cyberpunk2077.exe --v1_metrics --track_frame_type --output_file presentmon.csv --timed 90 --terminate_after_timed --no_console_stats
python ./tools/analyze_presentmon.py presentmon.csv --process-name Cyberpunk2077.exe --start-seconds 15 --end-seconds 75 --frame-generation-off --output presents.json --frames-csv frame-intervals.csv
python ./tools/analyze_runtime.py gpu-timings.csv --warmup 120 --segment 0 --output runtime.json
```

The PresentMon helper requires one process/PID/swapchain. If a capture contains
multiple swapchains it reports the identities and requires `--swap-chain`;
it does not choose the busiest one automatically. It accepts v1
`MsBetweenPresents` and derives v2 intervals from CPU start timestamps.
The pinned v2 CSV writer's default `CPUStartTime` is seconds; explicitly use
`--cpu-start-unit milliseconds` for a producer that writes that column in ms.
Use `--qpc-start`/`--qpc-end` for absolute QPC bounds and `--qpc-frequency`
for tick columns. V1 `QPCTime` requires `--qpc-unit ticks` or `milliseconds`
because its header does not distinguish the two capture options. Time ranges
include only intervals with both endpoints inside the inclusive range.
The first unavailable interval and any explicit warmup/range exclusions are
reported. Dropped or unshown presents, unknown frame types, zero intervals
after the first event, and timing outliers remain in the measurement. Only
explicit `AMD AFMF`/`Intel XeSS-FG` rows are removed, with intervals bridged
between retained application timestamps. `--frame-generation-off` conflicts
with those generated labels and enables the real-rendered FPS label; it is an
assertion about capture settings, not proof from CSV. Frame-type tracking
depends on application/driver instrumentation. This tool cannot detect
uninstrumented frame generation.

The optional runtime trace is enabled by creating an empty
`open-nr/record-timings.flag` before runtime creation. It appends
`open-nr/gpu-timings.csv` after completed NR jobs. Remove the flag before a
normal run, and move the old CSV aside between benchmark captures. The runtime
helper reports GPU pack/preprocess/network/composite/unpack and bridge timing,
sampled process-local DXGI memory, and observed cumulative counter changes.
It splits frame-ID/counter regressions into numbered segments and applies
warmup to completed jobs in each selected segment. Memory updates every 60
completed jobs; `-1` means unknown and repeated values are not independent
samples. Allocation bytes are partial neural activation/history storage.
Counter snapshots cannot place bypasses on individual rows, and the first
counter interval is unknown unless an excluded warmup row supplies a baseline.
Jobs missing from the trace, activity after its last row, and early bypasses
are not reconstructed. Runtime bridge timing may include GPU waits.
PresentMon and runtime statistics remain separate; trace row numbers or frame
IDs must not be joined to invent per-game-frame inference times. Runtime
logging adds CPU file I/O after completed GPU work, so record whether it was
enabled and repeat final performance captures without logging if overhead
matters. All generated reports refuse existing output paths.

For a separate diagnostic run, the runtime's opt-in `open-nr/capture.flag`
requests one snapshot when armed. Remove the flag across at least one enqueue,
then recreate it to re-arm a later capture. It copies the packed input,
preprocessed features, previous history, network head, and composed scene RGBA
from the GPU into a uniquely named frame/submission/process/time folder under
`open-nr/captures/`, with
controls and hashes in `manifest.json`. The composed image is Vulkan float32
before the Direct3D RGBA16F conversion, so it does not verify the final texture
publication or FSR/display output. Capture allocation/readback/file I/O is
excluded from performance testing; remove the flag and use a fresh normal run
for FPS measurements. Normal submission has no image readback. A capture or
repeatable replay alone does not establish NVIDIA parity or visual quality.
Local images/history may contain game content and should remain outside source
archives and distributable packages.

The current machine, game settings, captures and measured results are recorded
in [rx9070xt-validation.md](rx9070xt-validation.md). Keep that record with the
matching source and binaries. Use the following fields for additional runs;
this is a reporting template, rather than a claim that existing measurements
are absent. Do not substitute upstream NVIDIA numbers.

| Field | Required run record |
| --- | --- |
| GPU, driver, OS, adapter LUID | Identify the actual adapter and software versions |
| Game version, save, settings, camera path | Record a repeatable scene and configuration |
| Model DLL digest, source revisions, shader build | Pin the model, source and binary hashes |
| Display / actual render resolution, HDR / SDR | Record output and model input separately |
| Warmup and measured frame count, number of repeated runs | State exclusions and the retained interval |
| NR off frame time mean / p95 / p99 | Capture the matched baseline |
| Reference frame time and matched output | Keep reference and game performance scopes separate |
| AMD frame time mean / p95 / p99 | Use real application presents with frame generation off |
| GPU pack / inference / unpack time | Report completed runtime jobs separately from game presents |
| Peak VRAM and resize stability | Measure process GPU memory and repeat resize checks |
| MAE / RMSE / PSNR / SSIM / maximum error | Compare frames with matching model, controls and history |
| PSNR ≥ 40 dB and SSIM ≥ 0.99 over matched composed RGB sequences | Report every matched frame and any failed threshold |
| Bypassed measured frames and reasons | Retain all game frames and report runtime counter deltas |
| Temporal, exposure, cut, disocclusion, HDR checks | State tested cases and remaining gaps |
| 1440p output / 60 rendered FPS release gate | Keep unmet until all agreed acceptance checks pass |

## Source and licenses

The original `src/` neural core and original shader work keep their MIT license.
The importer keeps the MIT extractor's copyright and full notice. The pinned
OptiScaler host is GPL-3.0; compatibility declarations and the derived game
wrapper retain their GPL notices in `game/` and `integrations/optiscaler/`.
The distributed combined game DLL and host must be accompanied by their
corresponding source and build scripts under the applicable GPL terms. The
package puts those trees under `source/` and preserves license files. Keep that
source matched to the shipped binaries, including any later local changes.
Dependencies retain their own notices. The MIT core's separate license does
not remove the distribution obligations of the GPL wrapper/host.

This project is not affiliated with AMD or NVIDIA. Model compatibility does
not grant rights to NVIDIA's binaries, weights, or trademarks. Do not include
locally imported weights, game assets, or captures without the necessary rights
in a source archive or release.
