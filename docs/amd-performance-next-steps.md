# RX 9070 XT remaining performance work

## Current continuation status

Alpha 6 is the latest alpha package. Its explicit Pair/Arena/K16 route with
C32 QKV normalization on measures **47.74616 ms median network inference** at
1707×960, padded to 1728×960, versus **50.41260 ms** with that normalization off.
This is a 5.28923% same-build improvement. See the
[QKV normalization delivery](amd-qkv-normalize-delivery.md) and
[alpha 6 release notes](releases/v0.1.0-alpha.6.md) for identities, timing,
checkpoint, history and lifecycle qualification.

The 8 ms NR-plus-bridge and 16.67 ms complete-frame targets remain unmet.
Alpha 6 has no new complete-game FPS measurement. NR remains disabled by
default and the existing automatic tuning cache remains unchanged; an explicit
qualified route is not the automatic default. The alpha 3 game numbers retained
below apply to its older runtime and do not measure alpha 6.

The [later expansion screens](amd-strided-expansion-screen.md) retain
measured rejections, including v12's 0.41–1.21% operator gains, below the required
5%. The [expert input-sharing screen](amd-expert-a-reuse-screen.md) passes its
recorded byte checks but regresses every timing pair; its eight pooled operator
medians are 16.87–17.43% slower. These screens do not qualify a production graph.
The [cooperative publication screen](amd-cooperative-publication-screen.md)
also passes its recorded byte comparisons but regresses all seven pooled
operator medians by 3.26–3.98%. Its static-scope follow-up only restores baseline
operand-load overlap, retains the conversion work and increases offline VGPR
use; it stops at CPU review without a new host or GPU run.
The [SiLU inner publication screen](amd-silu-inner-normal-screen.md) passes its
fresh bounded, generated inner-boundary and actual graph-input byte checks.
Its seven pooled medians improve by 1.157–3.801%, with p95 regressions in blocks
1 and 69; all 21 paired median gains also remain below 5%. It stops at the
operator gate without a complete candidate graph or release change.

The separate [alpha 6 candidate profile](performance/qkv-normalize-measurements/profile-target-candidate.json)
records 503 dispatches. Median per-frame family totals are **30.09466 ms for FP8
GEMM**, **6.59034 ms for window attention**, **3.33166 ms for F16 GEMM** and
**3.12548 ms for QKV normalization**. These instrumented spans guide the next
experiment; they are not ordinary inference or game timings, and adding family
medians does not reconstruct a measured frame.

The next preserving fusion prototype targets expert expansion followed by
contraction in the C64/C128/C256 families. Those 72 dispatches have a median
same-frame summed span of **6.579260 ms** in that profile. The prototype keeps
the hidden E4 publication in shared memory and leaves the following projection
and residual placement unchanged. It must reproduce each original N16
publication phase on the tested driver before timing. This is a candidate cost
bound, with no qualified speed or quality result.

Separately, [end-of-K32 arithmetic](amd-c32-expansion-arithmetic-screen.md)
failed four of six matched SDR/HDR composed frames. The
[native-half operator screen](amd-native-half-arithmetic-screen.md) documents
changed arithmetic; its subsequent
[ordinary7 composed-frame screen](amd-native-half-graph-quality-screen.md)
passes the three generated SDR cases but fails all three HDR-highlight cases.
Both arithmetic routes remain excluded from promotion. Their references are
frozen AMD outputs, not independent original NVIDIA captures.

Next work keeps full internal resolution and the native Vulkan graph:

1. Capture instruction traces of the qualified Pair/Arena/K16 path on the
   current driver. Keep profiler runs separate from ordinary timing, record
   automatic profiler clock changes and restoration, and verify event geometry
   and trace completeness before interpreting instruction stalls or utilization.
2. Use the observed bottleneck to choose the next preserving fusion, scheduling
   or memory-reuse candidate. Smaller LDS/register counts and fewer logical
   memory loads alone have not established a speed improvement.
3. Keep further arithmetic changes opt-in and require every matched unclamped
   scene-linear RGB frame to pass 40 dB PSNR and 0.99 SSIM, including highlights
   and temporal cases. Preserve GPU input conversion in any later timing span.
4. Require applicable checkpoints, target output, identical and evolved histories,
   lifecycle and complete-inference timing gates before integration. Follow with
   warmed game benchmarks and active gameplay; network-only time does not
   establish game FPS.

The rejected candidates do not change shipping kernels, the qualified cache or
alpha releases. NVIDIA DLLs, extracted weights and captures remain excluded
from commits and release packages.

## Historical alpha 3 backlog and measurements

The following record preserves the measured state and pending work at alpha 3.
Use the current continuation status above for the latest alpha release.

The preserving direct-GEMM change lowers ordinary target inference from
**119.143 to 81.049 ms**, a **31.97%** reduction against qualified Q32/shared GEMM.
The 8 ms NR-plus-bridge and 16.67 ms complete-frame targets remain unmet.
The alpha 3 direct-GEMM runtime has three warmed Cyberpunk passes per
condition: **97.12269 FPS NR off / 10.69410 FPS NR on**. See the
[alpha 3 game record](performance/cyberpunk-alpha3-20261002.json) for current
game measurements and pending evidence. Three separately bounded asynchronous
runtime brackets have NR-plus-bridge medians **85.84388 / 85.94782 / 85.90282 ms**;
these completed-job spans are not joined to individual game presents. NR
remains disabled by default.

The current source adds a direct operand GEMM route. Its clean public operator,
320 checkpoint/head, target output and independent FFN32/QKV32 checkpoint
comparisons pass. Ordinary interleaved timing, the native eight-slot lifecycle
harness, automatic cache GPU selection and bounded identical/evolved replay
also pass. Three warmed game benchmarks per condition are complete. See the
[GEMM continuation record](amd-gemm-delivery.md); prototype screening does not
replace final timing or the published game's results.

The next kernel work should follow the new measured costs. In the separate
direct-GEMM instrumented target profile, FP8 GEMM has a **48.796 ms** family
median and window attention **21.033 ms**, with an **80.805 ms** whole-frame
median. Window normalization is **4.571 ms**, F16 GEMM **3.183 ms**, and global
attention **2.212 ms**. These family medians describe independently
summed per-frame spans; adding family medians does not reconstruct a frame.
They are not ordinary inference measurements or complete game frame times.
The [implementation record](amd-performance-implementation.md) and
[direct-GEMM scalar evidence](performance/direct-gemm-rx9070xt-20261002.json)
retain the measured identities, protocols and exact qualifications.

## Implementation work versus evidence gates

| Item | Current source or evidence | Remaining work |
| --- | --- | --- |
| Independent AMD FFN and QKV capabilities | Independent FFN32/QKV32 CLI/environment controls and actual evidence fields are implemented. Each direct-GEMM route independently passes all 75 checkpoints, head and composition at 320. | Measure each route alone and in combination at target geometry; qualify target output, runtime lifecycle and captured histories before promotion. |
| C32 pooling/upsampling variants | AMD C32 body fusion leaves adapters, transitions, pooling and the head outside; the native graph disables the existing NVIDIA pre/pool/up/post fusion routes. | Add separate AMD pooling/upsampling variants when their measured costs justify the next experiment. Retain exact adapter/head operations and the legal decomposed fallback. |
| Split-FFN stages | Native expert fusion covers C64/C128/C256; the C512 split-FFN path still materializes its separate GEMM boundaries. | Profile the actual split shapes, then add and qualify native fusion for measured hot stages, preserving each E4/half publication and residual boundary. |
| Installed-driver matrix delivery | Offline RGA covers all 420 target GEMM/attention dispatches. The inspected RGP Q32 event confirms wave32, resources and static FP8 WMMA; SQTT is truncated and no wavefront instruction timing was analyzed. | Obtain bounded installed-driver GEMM/attention traces with sufficient trace coverage to inspect operand delivery, occupancy, publication cost, barriers and queue waits. Keep captures separate from ordinary timings. |
| Faster preserving publication | Packed hardware conversion failed 780 of 858 strict checks; offline ISA uses round-toward-zero where software publication requires round-to-nearest-even. | Probe a correctly rounded alternative against the existing conversion oracle and witnesses before measuring it. Keep the failed packed path off. |
| Broad image/temporal acceptance | Eight existing fixed-camera SDR frames pass both history-mode numerical gates with the new Direct route; 80 buffer checks reproduce the final alpha2 Shared candidate in both modes. Earlier moving-alley frames are separately identified. Synthetic HDR-highlight cases still fail; broad event coverage and temporal review remain incomplete. | Localize highlight errors with checkpoints, capture demonstrated scenes, compare every unclamped scene-linear RGB frame and review motion for flicker/ghosting. Fixed-camera numerical passing does not establish movement or camera-cut coverage. |
| Game acceptance | Six warmed alpha3 benchmark passes and three asynchronous runtime brackets are complete. Sampled DXGI process-local memory has maximum 9,428.008 MiB; sparse samples are not a true interval peak. No alpha3 PresentMon was collected. The active 600-second gameplay session was not run. | Complete a demonstrated active world session with observed movement/actions, excluding menus/loading. A network/operator speedup or stationary interval does not establish the active-gameplay or game FPS target. |
| Malformed tuning JSON | The post-alpha2 fix rejects malformed literals, duplicate decoded keys, invalid strings/escapes/UTF-8 and invalid or unrepresentable numbers, with bounded nesting. New core/runtime, source rebuild, CPU parser/policy and native tests pass. | Retain the regression checks for future source changes. The published alpha2 assets retain their original parser and are unchanged. |

Independent selection now closes the earlier FFN/QKV enablement gap.
[The policy](../src/amd_config.h), [route predicates](../src/kernels.cpp),
and [graph](../src/nr_graph.cpp) retain separate expert/C32 body routes and
the exclusions of NVIDIA scheduling/fusion. The split-FFN and transition
extensions still need separate AMD implementations and measured justification.

## Order of the next experiments

1. **Reprioritize the measured costs after the direct GEMM qualification.**
   Use the actual model shapes and K16/N16/stage16 policy against the qualified
   Q32/shared baseline. Native lifecycle, automatic selection and bounded replay
   pass; retain those gates for each next change. Inspect the new separately
   instrumented profile and installed-
   driver operand delivery and publication after the observed improvement.
   Nine earlier tile/staging previews did not qualify a faster default;
   N64/stage64 still fails strict preservation.
2. **Isolate conversion and fusion changes.** Test correctly rounded half
   publication independently. Measure the independent FFN and QKV controls
   before redesigning the slower combined fusion route. Extend fusion to measured
   split-FFN/expert hot stages without enabling NVIDIA scheduling or discarding
   logical layer publications.
3. **Return to attention and small operators using the updated profile.**
   Q32 attention costs 21.033 ms in the new direct instrumented family series.
   Preserve physical key order, priors, the prescribed softmax tree, shifts
   and padding when reducing its storage or publication cost. Pooling and
   upsample/residual families currently cost approximately 0.259 and 0.107 ms,
   so their fusion alone cannot close the current budget gap.

Each preserving candidate needs strict operator bytes, all 75 checkpoints and
the F32 head at 320, target output, and production/decomposed equivalence.
Changed arithmetic stays opt-in and additionally needs every matched composed
frame to pass PSNR >= 40 dB and SSIM >= 0.99 at fixed data range 1.0, including
scene-linear highlights and both history modes. Repeat bridge/lifecycle tests
after fusion. Promote only with the prescribed interleaved ordinary target
timing and complete-inference regression gates; preserve selected-policy,
device/driver/model/shader and binary identities throughout.

Run `pwsh -NoProfile -File tests/test_json_parser.ps1` for **167 CPU checks**,
including the shipped tuning cache and an isolated `trux` rejection/fallback
fixture generated from it. Optional `-ModelManifest <local-path>` adds a valid
private model metadata check; the local run passed **168 checks**. The
earlier parser-stage policy validator passed **75 checks** with its shipped valid
cache and rejected that malformed file visibly; the current selector passes
**125 checks**. Baseline-versus-new parsing of
**36 actual JSON files / 208,437 typed nodes** preserves binary64 bits, string
bytes and structures. The parser intentionally limits nesting to 512 levels
and numbers to representable finite binary64 values. These CPU checks are
source-hardening evidence. New alpha3 core/runtime, source rebuild and native
validation include the fix; published alpha2 binaries and earlier measurements
remain unchanged.

New source/kernel experiments require new qualified binaries and tuning
evidence. The existing alpha 2 tag, source snapshot and downloadable assets
remain unchanged. This backlog makes no latency or FPS promise and keeps the
full internal-resolution, one-pass native Vulkan target.
