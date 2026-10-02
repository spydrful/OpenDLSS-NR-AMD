# RX 9070 XT validation record

This is a development implementation based on commit
`9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1`. It has not passed the requested
game image-quality or 60 FPS acceptance criteria. Generated models and captures
are local validation assets and are excluded from source control and packages.

## Machine and assets

Measured on Windows 11 Pro 10.0.26200, an Intel Core i9-12900HK, 96 GiB RAM,
and an AMD Radeon RX 9070 XT with Adrenalin 26.9.1 (LLPC), Vulkan 1.4.349.
Cyberpunk 2077 is version 2.31 (executable file version 3.0.5294808).
The OptiScaler source pin is
`557bb8553098395f5f138c2e22ed25f256f7a3a2`.

The supplied model DLL SHA-256 is
`6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`.
Its container version is 310.8.SF.0 (numeric PE version 310.8.2.0).
Its complete WEIGHTS_HT resource matches the original 310.8.0 resource digest
`836f445d06ecd2e59bb9f17b84b91c143396fd76ccda1c9dc7fe81d5edd548f4`.
Import produces 153 tensor records across 71 blocks and 11 native stage files.
The imported manifest digest is
`163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e`.

## Arithmetic and bridge evidence

The independent direct WGSL diagnostic and portable Vulkan reference match
bit for bit at all 75 captured model boundaries and the complete F32 head:
76 checks, zero failures, with real model bytes and deterministic 320 by 320
inputs. Direct block-zero diagnostics also match all nine intermediates.
This establishes agreement for these tests; it does not establish parity with
an original NVIDIA runtime. Original NVIDIA forward captures are still needed.
The upstream optimized browser implementation has Radeon numerical differences;
the direct arithmetic diagnostic is selected explicitly for reference checks.

The accelerated AMD normalization and global-attention changes preserve all
75 tested boundary buffers and the head bit for bit relative to the preceding
AMD implementation. These optimization regressions are separate from the
comparison between accelerated and exact arithmetic.

The controlled D3D12 harness has exercised shared buffers and timeline fences,
adapter LUID matching, eight queued frames, cancellation, unsafe submission
refusal, resize/reset/drain/shutdown, input rectangles and motion conventions,
GPU exposure, unsupported flags/formats, failed-prepare resource cleanup, and
host command-list binding restoration. Eight prefetched ordered submissions
match serialized RGBA16F output byte for byte. Prepared jitter ancestry and
submission-time history checks cover cancellation, skipped frames, reset and
resize, including an older queued frame consuming a reset before the next
preparation. Each lazy slot owns independent descriptors through consumer
completion. Model-backed output is finite and
preserves alpha. Harness evidence does not establish game-hook correctness.

An opt-in harness capture reproduces the runtime's F32 network head and
composed output bit for bit when replayed on AMD. All 409,600 captured channels
also reproduce the actual D3D12 RGBA16F texture using resource-format truncation
toward zero and finite overflow saturation. IEEE half round-to-nearest would
not reproduce this texture. Exact-reference replay of the same synthetic
captured source/history/controls produces 46.2448 dB PSNR and 0.992845 SSIM
against AMD in raw scene-linear RGB; predicted RTZ half publication gives
46.2418 dB and 0.992840. This is a harness capture, not a game capture.

## Measured model diagnostics

With a warmed, otherwise idle RX 9070 XT, the accelerated core median is
23.570 ms for 320 by 320 inputs and 217.949 ms for 1707 by 960 valid inputs
(padded to 1728 by 960). The latter is approximately the internal resolution
for 2560 by 1440 FSR Quality. These are network diagnostics, not game FPS.
They exceed the requested 8 ms budget for inference plus its bridge.

Scratch reuse reduced the graph's reported activation allocation at the target
resolution from 3513.08 MiB to 2222.88 MiB. This excludes model weights, game
resources and shared bridge buffers and must not be presented as total VRAM.

Matched synthetic scenes use the shipping preprocessing and composition
shaders, the same inputs, controls, model and prior history, and unclamped
scene-linear RGB with fixed PSNR data range 1.0. SSIM uses an 11 by 11 Gaussian
window, sigma 1.5, valid interior, averaged over RGB.

| Synthetic case | PSNR dB | SSIM | Meets 40 dB / 0.99 |
| --- | ---: | ---: | --- |
| SDR reset | 47.6907 | 0.997861 | Yes |
| SDR temporal | 44.2452 | 0.993310 | Yes |
| HDR reset | 32.6024 | 0.990710 | No |
| HDR temporal | 35.6995 | 0.986102 | No |

Highlights are retained, with reference values reaching about 17.4. The HDR
errors are not hidden by clamping or by normalizing PSNR to each scene's
maximum. These are synthetic scenes; real-game composed RGB and motion review
are additional requirements. HDR display support remains unvalidated.

## Cyberpunk measurements

Two early smoke runs are excluded. The first sent an unsupported legacy debug
flag and submitted no NR jobs. The second exposed the original two-slot limit:
every third preparation declined and reset history. The host now removes its
legacy debug flag for OpenNR, and the tested runtime uses eight lazy slots.
Neither invalid smoke is a neural game-performance or temporal-quality result.

A pre-install warmup of the ordinary game reported 104.96 average FPS,
75.17 minimum FPS and 138.40 maximum FPS over 64.25 seconds / 6744 frames.
This is an NR-off warmup, not one of the required three NR measurement passes.
The settings were 2560 by 1440, FSR 3 Quality, frame generation off, ray/path
tracing off, HDR None, and High graphics fields; the game labels this edited
configuration Custom. It must not be described as a verified unchanged High
preset. The accompanying PresentMon trace started late and includes the
results menu, so its complete trace is not a benchmark FPS measurement.

One complete NR-on warmup preceded three measured built-in benchmark runs in
the same game process. Each measured run used one accelerated NR pass at
1707 by 960, padded to 1728 by 960, before FSR. Effect and colour strength were
1, highlight guard 4, Standard style, tone/structure/white point 1, automatic
skin mask, and motion/history enabled with history strength 1. GPU timing
logging was enabled; diagnostic image readback was disabled.

| Built-in benchmark | Average FPS | Minimum FPS | Maximum FPS | Seconds | Frames |
| --- | ---: | ---: | ---: | ---: | ---: |
| NR-on warmup, excluded | 4.56 | 4.39 | 4.65 | 213.00 | 972 |
| Measured pass 1 | 4.56 | 4.53 | 4.60 | 213.15 | 972 |
| Measured pass 2 | 4.56 | 4.52 | 4.59 | 213.19 | 972 |
| Measured pass 3 | 4.57 | 4.53 | 4.60 | 212.90 | 972 |

PresentMon 2.3.1 recorded each measured pass separately. Loading, menus and
results are excluded by selecting seconds 100 through 200 of each capture;
screen observations confirmed the benchmark world in these intervals. Both
interval endpoints must fall inside the range. Timing outliers and unshown
presents are retained. The target is PID 31080, one Cyberpunk swapchain.

| PresentMon benchmark segment | Intervals | Application FPS | Mean ms | P95 ms | P99 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pass 1 | 455 | 4.5667 | 218.9759 | 221.5356 | 222.8159 |
| Pass 2 | 455 | 4.5625 | 219.1762 | 221.7961 | 222.4221 |
| Pass 3 | 456 | 4.5728 | 218.6836 | 221.3485 | 222.4381 |

In-game frame generation is off in the settings and benchmark results.
Driver AFMF could not be verified: access to Adrenalin timed out. PresentMon
therefore reports application-presentation FPS and leaves the real-rendered
FPS assertion unset. No generated-frame throughput is used as game FPS.
The 16.67 ms/60 FPS goal is plainly unmet under either interpretation.
The completed runtime trace reports zero cumulative NR bypasses through the
three measured runs. Runtime job IDs are not joined to PresentMon intervals.

The local files `build/validation/cyberpunk-neural-pass1` through `pass3`
retain the raw CSVs, selected intervals, PresentMon reports and transcribed
built-in results. Raw CSV SHA-256 values are respectively
`cbff97f2cc1e9eea6c7cb1c6aafa54eec9d3298ba607e4ab05be649e392fe16e`,
`ac46ced4c2c9136b0ce10c30b5f4c6f14d2681c655c60440a22f80baa00ca47e`, and
`0260bc6128bc4f4b9a0f698c2c8e96872d5eea943d9487aa3b17e7220598027c`.

The measured runtime DLL SHA-256 is
`fbd056d54ed6569a95ed7bf2060a7dd575249bcaea69a9d9fd47fab546d1a7ed`;
the patched host is
`3c43bb1d4a5843b68b0537b8703c4a0c12bf4e47f17c53e225df57cad276dee7`.
The diagnostic executable is
`21618190e8535ba0b2d5ffecb4f8ec8b05ff37c1a983b3770200a66f1355f499`.
Host product version is `0.4.8-amd-nr (557bb855+OpenNR)`. Dependencies are
FidelityFX loader 2.3.0.2740, upscaler 4.1.1.2740, and DirectX Agility
1.615.1.0.20250213.1. Build tools are Visual Studio 2022 Build Tools/MSVC
19.44.35228.0 (toolset 14.44.35207), Windows SDK 10.0.26100.0, glslang 16.6.0,
Vulkan-Headers 1.4.363, volk 1.4.357, CMake 3.31.12 and Ninja 1.13.2.

The original user settings are preserved in a local backup. The following
live-world check and diagnostic captured-input comparisons are recorded
separately from these three performance runs.

## Ten-minute live-world session

After the three benchmarks, the same process loaded the existing local
War Pigs / AutoSave-11 save, on a night street near an overpass. NR was visibly
enabled before PresentMon started. The capture contains a selected 600-second
interval, seconds 15 through 615, with 2758 complete application-present
intervals. Image capture was disabled throughout this performance interval.

| Duration | Application FPS | Mean ms | Median ms | P95 ms | P99 ms |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 600 seconds | 4.5971 | 217.5270 | 218.4928 | 221.7986 | 222.9616 |

The minimum and maximum intervals are 181.3018 and 250.3632 ms. No explicit
generated frames, dropped presents or zero intervals were present; outliers
were retained. Driver AFMF remains unverified, so this is application FPS and
the real-rendered FPS field remains unset. The raw PresentMon CSV SHA-256 is
`58a17dba236812eb93e6753dd15bc67274d3c0a083b9270929c9ccae167c19a5`.

This was a limited stability session, mostly stationary, with one short
automated movement input and some detected manual input. It is not a
controlled camera path or a completed review of faces, disocclusion, flicker,
ghosting, cuts or moving objects. No broad gameplay-quality acceptance is
inferred from the ten-minute duration.

## In-game GPU timing and memory

A preserved prefix of `gpu-timings.csv`, taken before enabling image capture,
contains 8160 completed NR jobs. Its SHA-256 is
`f24bc5d7fefd0c61b6c64287225c8953d5ff49e1be6dd04486e66f2136078e43`.
The analysis removes 30 warmup jobs from each of five detected segments.
Frame-ID regressions split segments; these segments are not individual
benchmark runs and are not joined to PresentMon frame intervals. The prefix
covers warmup, benchmarks, menus and the live-world session.

The last segment contains 3172 measured completed jobs, including live-world
entry and adjacent frames. Its stage medians are:

| Stage | Median ms |
| --- | ---: |
| D3D12 input packing | 0.04976 |
| Vulkan preprocessing | 0.46468 |
| Neural inference | 212.01616 |
| Vulkan composition | 0.32092 |
| D3D12 output conversion | 0.06252 |
| NR plus bridge elapsed GPU span | 213.316875 |

The last segment's NR-plus-bridge P95 is 214.6186465 ms and P99 is
215.0330892 ms. The span can include handoff waits, so it need not equal the
sum of stage medians. It exceeds the requested 8 ms budget by over 26 times.
Inference dominates; the bridge is not the main performance bottleneck.

Peak sampled DXGI process-local VRAM usage across the entire preserved trace
is 9702.01953125 MiB (about 9.475 GiB), including the game. The last segment
peaks at 9464.6328125 MiB. Samples refresh every 60 completed jobs and cannot
prove instantaneous maximum VRAM. The separate 2383296512-byte reported
neural allocation covers activations and history only. Ending counters show
8162 submissions and zero cumulative NR bypasses; two submitted jobs were
not yet represented in the completed-job prefix. No game FPS is derived
from these counters.

## Capture provenance and remaining gates

Diagnostic capture is opt-in and was enabled only after performance
measurement. Its files contain packed inputs, previous history, controls,
features, model head and composed scene-linear output, with hashes and model
and shader identity. Ordinary frames do not read image data back to the CPU.

The first live-world capture was incorrectly labeled `gameCapture=false`
because Windows returned a lowercase executable basename. The manifest has
been preserved without relabeling and is not accepted by the strict genuine
game-capture replay tool. A case-insensitive executable-name comparison fixes
the capture classification. The rebuilt runtime SHA-256 is
`5307d3dffa333e334db93d11a53acf939cc8a14c7ac319fe97e6a95fc2309516`.
This metadata-only change does not alter shaders, arithmetic or submission.
The three benchmarks and ten-minute trace used the earlier runtime hash
listed above. A corrected genuine-game capture and exact-reference replay
remain pending; synthetic and harness comparisons do not substitute for them.

Offline inspection of the unchanged, misclassified capture verifies all six
file sizes and SHA-256 values, and links its model and shaders to the tested
assets. Source, composed RGB, head and features are finite; alpha is preserved.
Source RGB reaches 49.0625 and composed RGB reaches approximately 23.928,
confirming that this captured frame retains values above display white.
This check is not an accelerated-versus-reference quality comparison.

A separate numerical replay of that unchanged capture succeeds at full
1707 by 960 valid resolution: AMD reproduces the captured feature, head and
composed RGBA bytes exactly, and repeat evaluation is identical. Exact
reference replay uses the same source, controls and captured AMD prior
history and reproduces the same preprocessing bytes. This is a matched
single-frame comparison; it is not a closed reference history trajectory.

| Preserved frame 8524 numerical comparison | PSNR dB | Gaussian SSIM |
| --- | ---: | ---: |
| Unclamped F32 scene-linear RGB, data range 1 | 50.944434 | 0.999813768 |
| Predicted D3D12 RTZ half RGB publication | 50.943438 | 0.999813318 |

Both comparisons contain 4916160 RGB samples and meet the numerical thresholds
for this one input. Alpha is preserved and no predicted half values saturate.
The maximum F32 absolute difference is 0.70805168, in a highlight; RMSE is
0.0028364706. These metrics remain classified as `capturedGameFrames=false`
and `syntheticScene=true` by the replay tools because of the unchanged source
manifest. They do not close the genuine game-capture or sequence-quality gate.
The local replay report is
`build/game-capture-validation/misclassified-liveworld-8524-replay/replay-summary.json`.
Reinhard/sRGB previews are diagnostic previews and do not reproduce the
game's tonemapper or display output.

The corrected-capture retry encountered a Windows desktop activation failure
(`GetCursorPos failed: Access is denied`, `0x80070005`) after one recovery
retry. Further game input was paused. This prevents claiming that the rebuilt
capture classification has been exercised in a new genuine game capture.

The project is a development implementation. The 60 FPS and 8 ms performance
goals are unmet, synthetic HDR scene-linear precision fails the requested
quality thresholds, and broad game motion review is incomplete. HDR display
validation and RX 7000 acceleration remain deferred as requested. Original
NVIDIA parity is not asserted without independent original forward captures.

## Restored test installation

After preserving the performance prefix, full timing log and captured buffers,
the owned benchmark process was stopped. The reversible installer removed the
temporary proxy, runtime, shader and dependency files and restored any
verified originals. The original user settings were restored byte for byte,
with SHA-256
`8cc2e801abff37e3ee07d62d0ea830ef164a9bc5a879eb639494562da373bd55`.
The capture and timing flags were removed. Separately imported model files
and unlisted local evidence were preserved. The development package is not
left installed in the user's game.

## Development package and build checks

The local delivery is `dist/OpenNR-AMD-rx9070xt-development`. It contains
`OpenNrRuntime.dll`, the patched OptiScaler host, 16 approved SPIR-V shaders,
the strict importer and model diagnostic executable, reversible installation
scripts, notices and corresponding source. There are 26 verified install
targets. The package excludes the supplied NVIDIA model DLL, generated
weights, game images and local capture fixtures. Its four release-gate fields
remain explicitly unmet.

The final package source was copied into a fresh validation directory and
rebuilt with the recorded toolchain: native core and shaders, D3D12/Vulkan
runtime, importer and OptiScaler host all compile successfully. The importer's
213 malformed-input/model-layout checks pass. Package validation has 34 passing
synthetic checks; the FSR31 source-symbol patch helper has six passing checks.
The local log is `build/host-safety/final-development-source-build.log`.

Five version-matched upstream static-dependency source archives are shipped
intact with length/SHA checks, alongside the pinned host's supplied linker
libraries and the checked FSR31 symbol-renaming patch. The build relinks
those supplied libraries; their original compiler options and byte-identical
reproduction were not established. This limitation is documented in
`integrations/optiscaler/sources/README.md`. The core retains its MIT notice;
the derived runtime and host carry the GPL source and notices.
