# Final alpha 2 Cyberpunk validation

This is the separate validation record for the published
`v0.1.0-alpha.2` binaries, tested on October 2, 2026. It does not replace earlier
benchmark files or change the release tag, assets or original release notes.
The [scalar evidence](performance/cyberpunk-alpha2-rx9070xt-20261002.json) contains
confirmed measurements, source hashes and explicit pending fields. Raw game
images, capture buffers, model weights and machine-specific logs remain local.

**Three warmed NR-off and three warmed NR-on benchmark passes are complete.**
Their equal-pass averages are **97.66045 FPS off / 7.54553 FPS on**. The complete
NR-on benchmark misses the 16.67 ms / 60 FPS goal. Bounded runtime NR-plus-bridge
medians are **124.953 / 124.964 / 124.952 ms**, missing the 8 ms budget. Closed
timing/memory analysis, eight-frame numerical replay and reversible cleanup are
complete. Ten minutes of active gameplay were not run, and broad image/temporal
acceptance remains unmet. NR remains disabled in the release configuration.

## Binary and setting identity

| Item | Tested identity or setting |
| --- | --- |
| Release source | `7f1cd3108133d8aee9bae505cb31542e236d13e0` |
| Runtime SHA-256 | `638de5aee97b65d5e091c5eb6af63df96cf38d729985cadff2b8cb79fe5c3e6c` |
| Diagnostic CLI SHA-256 | `ac87f20bf8483688388abcf94d82724b591e95874449bc194d90934d36b236f8` |
| Tuning SHA-256 | `6f2298c945eaa54527d9642a31a9d47d29ebd1d768a128442f9f9f4374c6017d` |
| Hardware/software | RX 9070 XT, Windows 11 Pro 10.0.26200, Adrenalin 26.9.1, Cyberpunk 2077 2.31 |
| Display | 2560 x 1440, SDR, Windowed, VSync and FPS limit off |
| Graphics | Custom configuration with target High fields and High textures; ray tracing/path tracing off |
| Game upscaler | FSR 3.0 Quality; dynamic resolution off |
| Frame generation | Game FG, driver AFMF and driver FSR FG override observed off |
| Driver upscaling override | Enabled and unchanged between conditions; effective upscaler version not independently established |
| NR-on geometry | One full internal-resolution pass before FSR, 1707 x 960 valid / 1728 x 960 padded; warmup and measured-run runtime logs |
| Observed NR-on policy | Optimized K16/N16/stage16/Q32, all fusion and hardware-publication flags off |

The [final identity record](performance/final-rx9070xt-binary-identities.json)
pins the remaining shader/tool identities. Installed binary identity and NR
condition are separately verified; the game's benchmark export does not itself
prove which neural DLL was installed. Game reports independently record game
FG, ray tracing, upscaler, display resolution and dynamic-resolution settings.

## Complete built-in benchmark exports

One NR-off warmup run was discarded before the three retained passes. The
separate final-runtime NR-on warmup is also preserved and excluded before the
three retained NR-on passes. Image capture is excluded from ordinary performance
runs. The frame-time statistics
below cover each complete game-exported CSV; they are not PresentMon subsets.
The game exports rounded frame times, so these percentiles retain that precision.

| Condition / pass | Average FPS | Frames | Duration | Median | P95 | P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| NR off / 1 | 96.09036 | 6,175 | 64.26243 s | 10.10 ms | 13.73 ms | 15.70 ms |
| NR off / 2 | 96.61504 | 6,207 | 64.24465 s | 9.95 ms | 13.637 ms | 15.1876 ms |
| NR off / 3 | 100.27594 | 6,444 | 64.26267 s | 9.60 ms | 13.3585 ms | 14.9557 ms |
| NR on / 1 | 7.54624 | 972 | 128.80591 s | 132.52 ms | 133.8945 ms | 134.7958 ms |
| NR on / 2 | 7.54468 | 972 | 128.83252 s | 132.58 ms | 133.9445 ms | 134.43 ms |
| NR on / 3 | 7.54567 | 972 | 128.81560 s | 132.54 ms | 133.84 ms | 134.5348 ms |

The NR-off equal-pass mean is **97.66045 FPS**, with descriptive sample standard
deviation **2.28022 FPS** and coefficient of variation **2.33485%**. Pooling all
**18,826 exported frame times** gives median **9.88 ms**, P95 **13.54 ms** and
P99 **15.34 ms**. The pooled percentiles weight actual frames; they are not
averages of pass percentiles.

The NR-on equal-pass mean is **7.54553 FPS**, with descriptive sample standard
deviation **0.00079 FPS** and coefficient of variation **0.01045%**. Its
**2,916 complete exported frame times** have median **132.55 ms**, P95
**133.89 ms** and P99 **134.637 ms**. NR-on average FPS is **92.27% lower** than
the matched NR-off equal-pass mean. These are actual complete game benchmark
results, not neural throughput or generated frames. Three repeats describe
observed variation; they do not establish a confidence interval or a speedup
against an earlier binary.

## PresentMon, runtime timing and memory

The closed PresentMon trace contains 643,728 rows for the observed game process
and swapchain. Five independently bounded world subsets are valid. Both
present-interval endpoints must fall inside the observed inclusive QPC bounds;
no corrected cutoff, outlier removal or runtime-frame join is inferred.

| Bounded world subset | Observed bound duration | Present intervals | Application FPS | Median | P95 | P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off-1 | 30.29143 s | 3,161 | 104.35786 | 9.59680 ms | 11.30000 ms | 12.43694 ms |
| off-3 | 28.47081 s | 3,008 | 105.67065 | 9.07800 ms | 12.69097 ms | 14.50357 ms |
| on-1 | 68.95932 s | 521 | 7.56614 | 132.22110 ms | 134.46000 ms | 135.59002 ms |
| on-2 | 69.58875 s | 525 | 7.56361 | 132.16140 ms | 134.39218 ms | 134.90276 ms |
| on-3 | 69.51000 s | 524 | 7.56237 | 132.20040 ms | 134.33999 ms | 135.48979 ms |

The raw hybrid trace contains both `TimeInQPC` and distinct `CPUStartQPC`
values. Direct `MsBetweenPresents` intervals measure present starts, so the
corrected helper qualifies boundaries with the matching `TimeInQPC` clock.
[PresentMon's pinned clock definitions](https://github.com/GameTechDev/PresentMon/blob/v2.3.1/PresentMon/OutputThread.cpp#L373-L386)
and actual trace witnesses confirm the mismatch. Eleven CPU diagnostic tests
pass, including hybrid-boundary and CPU-only-clock rejection cases. Original
reports and raw data remain unchanged; newly named clock-corrected reports
supersede their world-bound qualification. The third NR-on subset changes from
525 to **524** intervals; the complete engine benchmarks and runtime/memory
measurements are unaffected.

These are subsets, separate from the complete built-in benchmark exports. No
zero-duration intervals were observed in the selected subsets. The raw CSV has
neither Dropped nor FrameType columns, so dropped/generated observations are
unknown; no rows are filtered by those unavailable fields. Analyzer-reported
zero counts do not establish zero dropped/generated presents. Frame generation
off is externally verified; the CSV alone cannot establish that an
uninstrumented generator was disabled.

The second NR-off interval is excluded from world-only PresentMon analysis:
its end marker was observed after the results screen. Its original marker/CSV
remain unchanged, no corrected cutoff is inferred, and its complete built-in
report remains valid.

Three runtime brackets select rows published between the start/end complete-row
snapshots. They contain 526 / 540 / 494 completed jobs, each in one segment, with
no extra warmup removal inside the already warmed measured intervals. These
published jobs may have been submitted before a world boundary or completed
after it; their count is not an exact count of producer jobs inside the QPC range.

| Runtime bracket | Completed rows | Network median | NR plus bridge median | NR plus bridge P95 | NR plus bridge P99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| on-1 | 526 | 123.69896 ms | 124.95318 ms | 126.26470 ms | 127.19436 ms |
| on-2 | 540 | 123.69422 ms | 124.96404 ms | 125.91348 ms | 126.26607 ms |
| on-3 | 494 | 123.65546 ms | 124.95221 ms | 125.88132 ms | 126.31736 ms |

The scalar record retains every pack, preprocessing, inference, composition,
unpack and NR-plus-bridge distribution. GPU NR-plus-bridge spans may include
queue/fence waits; stage medians are not added to reconstruct an overall median.
Asynchronous before/after counter differences are 526 / 540 / 494 submitted
frames and zero bypasses in all three brackets. Activity outside those snapshots
remains unknown, and no per-present bypass association is inferred. NR-off has
no completed neural jobs; this absence is not zero inference time.

Sampled DXGI process-local VRAM maxima are **9,530.875 / 9,607.879 / 9,524.871 MiB**
in the three NR-on brackets. DXGI refreshes every 60 completed jobs; repeated
values are not independent samples and these maxima are not true peaks.
Runtime partial neural allocation is **2,383,296,512 bytes** in each bracket and
excludes model weights, game resources and shared buffers.

Separate Windows PDH point samples are:

| Pass | Dedicated / local, each | Shared |
| --- | ---: | ---: |
| off-1 start | 6444.371 MiB | 86.969 MiB |
| off-1 after pass | 6616.754 MiB | 26.969 MiB |
| off-2 start | 6405.973 MiB | 85.969 MiB |
| off-3 start | 6473.457 MiB | 77.969 MiB |
| on-1 start | 9625.883 MiB | 431.441 MiB |
| on-2 start | 9689.773 MiB | 421.441 MiB |
| on-3 start | 9627.844 MiB | 427.441 MiB |

These sparse OS samples do not establish peak memory and are separate from
runtime DXGI accounting. Dedicated, local and shared values must not be added
together. The original off-1 helper's derived totals were invalid; its raw
counters remain preserved, and a separate correction report supplies the
numbers above without changing the original file.

## Active gameplay and image review

The ten-minute active world session was not run; manual game input is required.
Automated control has not demonstrated sustained movement; stationary world observation
does not complete this test. Its record must distinguish
movement/actions from menus/loading and preserve observed duration, frame times,
failures, bypasses and sampled memory. Diagnostic image captures are separate
from ordinary performance evidence.

Eight genuine fixed-camera city-ramp frames, **7085–7092**, were captured as a
separate diagnostic. The CPU audit verifies 56 required files, controls, sizes,
hashes and declared ancestry. The first frame uses captured history from frame
7084; the remaining seven frames declare consecutive history. Source RGB reaches
**177.125**, and production-composed RGB reaches **153.17038**, without clamping.
Exposure is unavailable and uses fallback 1 throughout; this sequence does not
establish GPU exposure transitions or HDR display validation.

All **32 replay runs** succeeded: eight frames in two history modes, each with
an AMD candidate and the unchanged portable exact reference. The 16 candidate
runs select K16/N16/stage16/Q32 with fusion and hardware publication off. The
portable exact software schedule remains unchanged; the AMD Q32 selection does
not apply to the 16 reference runs.

Every matched composed RGB frame passes PSNR >= 40 dB and SSIM >= 0.99 at fixed
data range 1.0, in unclamped scene-linear pre-FSR RGB. Minimum values are
**51.32959 dB / 0.999792** with identical history and
**50.52491 dB / 0.999676** with independently evolved histories from the same
reset. Each evolved route carries its own composed history forward.

Independent streaming comparisons also reproduce captured source packing,
controls, features, F32 head and composed RGBA byte for byte in the
identical-history AMD replay: **40 checks / 1,690,952,384 bytes** over eight frames.
This verifies production reproduction with the selected accelerated policy; it
does not mean byte parity against the exact reference. Maximum absolute RGB
errors against that reference are **1.87991 / 1.90947** across the two modes,
so aggregate PSNR/SSIM passing still requires visual and localized highlight
review. Captured output is Vulkan F32 before D3D12 RGBA16F output conversion.

Coverage is **SDR only**: a fixed camera overlooking the city, with ambient scene
changes. Player movement was not established. Faces, camera cuts, disocclusion,
moving-object sequences, exposure changes and sustained camera motion are not
qualified by these eight frames. Broad coverage and human temporal review for
flicker/ghosting remain incomplete. Diagnostic readback is excluded from
ordinary performance measurements.

The [earlier genuine eight-frame replay](performance/cyberpunk-replay-rx9070xt-20261002.json)
passes those numerical thresholds for a short alley sequence. It uses earlier
application binaries and does not close broad scene coverage or temporal review.
Existing synthetic HDR-highlight comparisons still fail the quality gate. HDR
display validation remains deferred, while scene-linear highlights in the SDR
target remain part of numerical/image validation.

## Historical records and remaining gates

[The earlier optimized game record](performance/cyberpunk-rx9070xt-20261002.json)
contains 99.58 / 99.32 / 101.74 NR-off FPS and one 7.51 FPS NR-on pass from earlier
binaries. [The original validation record](rx9070xt-validation.md) retains alpha 1
and earlier development measurements. Neither is relabeled as a fresh final
alpha 2 result.

| Gate | Current final-alpha2 status |
| --- | --- |
| Three warmed NR-off passes | Complete |
| Three warmed NR-on passes | Complete |
| Ten minutes active gameplay | Not run; manual input required |
| Final game NR plus bridge <= 8 ms | Unmet: bounded runtime medians 124.953 / 124.964 / 124.952 ms |
| Complete NR-on frames <= 16.67 ms / 60 real FPS | Unmet: 7.54553 FPS mean; 132.55 ms pooled frame median |
| Eight final-alpha2 SDR frames, both history modes | Every numerical threshold passes; captured production reproduces exactly |
| Broad scene coverage and temporal visual review | Incomplete |
| Synthetic scene-linear HDR-highlight quality | Unmet |
| Reversible cleanup and original settings restoration | Complete; 35 managed files removed, original settings restored exactly |

Cleanup was verified at **2026-10-02T17:39:18.8169652Z**. The game and PresentMon
processes were absent, the installation ledger and owned timing/capture/trace
flags were absent, and the original settings SHA-256 was restored exactly to
`d1b2419f1da1a308f13b594b222d77144f5b42f0836b2ae92beefb0da18607c6`.
The imported model manifest remained unchanged; earlier and new local captures
were retained, and cleanup did not modify unrelated saves. The scalar record
retains the restoration-proof hash. This completes reversible cleanup without
completing the pending active-gameplay or quality tests.

The release stays a development alpha with NR disabled by default. Unperformed
measurements are unknown, never zero, and this document does not approve any
unmet release gate. The published tag, assets and original release notes are
unchanged by this later validation record.
