# RX 9070 XT remaining performance work

The qualified Q32 attention change lowers ordinary target inference from
**141.343 to 122.331 ms**, a **13.45%** reduction against legal compact Q64.
The 8 ms NR-plus-bridge and 16.67 ms complete-frame targets remain unmet.
The published alpha 2 binaries now have three warmed Cyberpunk passes per
condition: **97.66045 FPS NR off / 7.54553 FPS NR on**. See the separate
[final alpha 2 validation record](cyberpunk-alpha2-validation.md) for current
game measurements and pending evidence. Three separately bounded asynchronous
runtime brackets have NR-plus-bridge medians **124.953 / 124.964 / 124.952 ms**;
these completed-job spans are not joined to individual game presents. NR
remains disabled by default.

The next kernel work should follow the measured costs. In the separate
instrumented target profile, FP8 GEMM occupies approximately **74%** of the
candidate frame: **89.324 ms** of a **121.062 ms** whole-frame median.
Window attention is **20.854 ms**. These family medians describe independently
summed per-frame spans; adding family medians does not reconstruct a frame.
They are not ordinary inference measurements or complete game frame times.
The [implementation record](amd-performance-implementation.md) and
[legal-anchor scalar evidence](performance/legal-compact64-rx9070xt-20261002.json)
retain the measured identities, protocols and exact qualifications.

## Implementation work versus evidence gates

| Item | Current source or evidence | Remaining work |
| --- | --- | --- |
| Independent AMD FFN and QKV capabilities | Separate kernels and predicates exist, but `amdQkv32Enabled()` returns `amdFfn32Enabled()`. Both use the same `fusion` policy; expert and C32 body flags are separate. | Separate enablement and qualification so each route can be measured alone and in combination. Distinct CLI/environment spellings would be an implementation choice. |
| C32 pooling/upsampling variants | AMD C32 body fusion leaves adapters, transitions, pooling and the head outside; the native graph disables the existing NVIDIA pre/pool/up/post fusion routes. | Add separate AMD pooling/upsampling variants when their measured costs justify the next experiment. Retain exact adapter/head operations and the legal decomposed fallback. |
| Split-FFN stages | Native expert fusion covers C64/C128/C256; the C512 split-FFN path still materializes its separate GEMM boundaries. | Profile the actual split shapes, then add and qualify native fusion for measured hot stages, preserving each E4/half publication and residual boundary. |
| Installed-driver matrix delivery | Offline RGA covers all 420 target GEMM/attention dispatches. The inspected RGP Q32 event confirms wave32, resources and static FP8 WMMA; SQTT is truncated and no wavefront instruction timing was analyzed. | Obtain bounded installed-driver GEMM/attention traces with sufficient trace coverage to inspect operand delivery, occupancy, publication cost, barriers and queue waits. Keep captures separate from ordinary timings. |
| Faster preserving publication | Packed hardware conversion failed 780 of 858 strict checks; offline ISA uses round-toward-zero where software publication requires round-to-nearest-even. | Probe a correctly rounded alternative against the existing conversion oracle and witnesses before measuring it. Keep the failed packed path off. |
| Broad image/temporal acceptance | Eight final-runtime fixed-camera SDR frames pass both history-mode numerical gates, and 40 production-buffer checks match exactly. Earlier moving-alley frames are separately identified. Synthetic HDR-highlight cases still fail; broad event coverage and temporal review remain incomplete. | Localize highlight errors with checkpoints, capture demonstrated scenes, compare every unclamped scene-linear RGB frame and review motion for flicker/ghosting. Fixed-camera numerical passing does not establish movement or camera-cut coverage. |
| Game acceptance | Six final-binary warmed benchmark passes, five bounded PresentMon subsets and three asynchronous runtime brackets are complete. Sampled DXGI process-local memory has maximum 9,607.879 MiB; sparse samples are not a true interval peak. The active 600-second gameplay session was not run. | Complete a demonstrated active world session with observed movement/actions, excluding menus/loading. A network/operator speedup or stationary interval does not establish the active-gameplay or game FPS target. |
| Malformed tuning JSON | A post-alpha2 source fix rejects malformed literals, duplicate decoded keys, invalid strings/escapes/UTF-8 and invalid or unrepresentable numbers, with bounded nesting. CPU parser and policy tests pass. | Build and validate new runtime/diagnostic binaries before distributing this fix. The published alpha2 assets retain their original parser and are unchanged. |

The plan explicitly asks for “independent capabilities for AMD FFN,
QKV/attention and C32 block fusion.” The current kernels are independent of
NVIDIA/PTX routes, but FFN and QKV enablement is coupled. Separate command-line
flags are one way to close that gap; the literal request does not prescribe
their names. This distinction should remain clear when describing completed
backend isolation versus remaining per-route selection and qualification.

Source locations supporting the partial implementation are
[the FFN/QKV predicates](../src/kernels.cpp#L165),
[the shared policy flag](../src/amd_config.h#L15),
[the native fusion-route exclusions](../src/nr_graph.cpp#L151),
[the C32 body boundary](../src/nr_graph.cpp#L312), and
[the separate split-FFN path](../src/nr_graph.cpp#L552).
These are remaining implementation tasks, not a claim that the currently
qualified preserving Q32 runtime has failed its recorded byte comparisons.

## Order of the next experiments

1. **Resolve the GEMM cost first.** Use the actual model shapes and the current
   K16/N16/stage16 policy. Inspect installed-driver delivery and publication
   separately from operand staging. Nine tile/staging previews did not qualify
   a faster preserving default; N64/stage64 also fails strict preservation.
   Wider tiles are therefore candidates to measure, not assumed winners.
2. **Isolate conversion and fusion changes.** Test correctly rounded half
   publication independently. Give FFN and QKV independent enablement before
   redesigning the slower combined fusion route. Extend fusion to measured
   split-FFN/expert hot stages without enabling NVIDIA scheduling or discarding
   logical layer publications.
3. **Return to attention and small operators using the updated profile.**
   Q32 attention still costs 20.854 ms in the instrumented family series.
   Preserve physical key order, priors, the prescribed softmax tree, shifts
   and padding when reducing its storage or publication cost. Pooling and
   upsample/residual families currently cost approximately 0.270 and 0.108 ms,
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
recompiled native policy validator passes **75 checks** with the shipped valid
cache and rejects that malformed file visibly. Baseline-versus-new parsing of
**36 actual JSON files / 208,437 typed nodes** preserves binary64 bits, string
bytes and structures. The parser intentionally limits nesting to 512 levels
and numbers to representable finite binary64 values. These CPU checks are
source-hardening evidence; the existing GPU replay, game timings and published
alpha2 binaries were not rebuilt or relabeled for this change.

New source/kernel experiments require new qualified binaries and tuning
evidence. The existing alpha 2 tag, source snapshot and downloadable assets
remain unchanged. This backlog makes no latency or FPS promise and keeps the
full internal-resolution, one-pass native Vulkan target.
