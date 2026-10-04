# Expert W1–W2 shared-memory fusion screen

The private C64/C128/C256 W1→W2 prototype is **rejected for performance**.
It passes the evaluated strict bytes, but every one of the 36 model-weight
operators is slower: pooled medians regress **87.47–171.20%**, and all 108 paired
medians regress **83.22–178.61%**. No shipping kernel, release, default, qualified
cache or ABI changes. NR remains disabled by default.

The qualified alpha 6 explicit Pair/Arena/K16 route with C32 QKV normalization
on remains **47.74616 ms median network inference** at 1707×960, padded to
1728×960. This screen adds no complete-inference or game measurement. The
8 ms NR-plus-bridge and 16.67 ms complete-frame targets remain unmet.
See the [scalar record](performance/expert-w1-w2-lds-rx9070xt-20261004.json)
for all 6,480 measured samples, pooled median/p95/p99/variation and three paired
statistics for each operator. The
[qualified QKV record](performance/qkv-normalize-rx9070xt-20261003.json)
retains the ordinary network identities and measurements.

## Candidate and baseline

The baseline uses the original repaired Pair kernel with native E4 FP8
operands and F32 accumulation, ordered K16 publication, N16/stage16, separate
W1 expansion with original SiLU and E4 publication, then W2 contraction and E4
publication. The candidate performs those two stages in one 128-thread
workgroup per expert and 64 rows, holding the hidden E4 result in shared memory.
W3, its residual and the rest of the graph are untouched.

The prototype keeps separate software publication state for each original N16
tile/logical wave and reproduces the tested AMD publication visits. It retains
the original SiLU and opaque raw-positive-zero seed; the rejected SiLU shortcut
is not included. This frozen-driver compatibility path is qualified only by the
reported byte comparisons and is not an arbitrary-input or original NVIDIA
parity claim.

Installed-driver production VGPR/SGPR counts are 88/40, 62/47 and 74/48 for
C64/C128/C256; capture counts are 93/51, 67/58 and 79/59. All use 16 KiB LDS with
zero scratch allocation/static scratch instructions, native FP8/F32 WMMA and no
hardware E4 conversion or MODE setter. The host explicitly requests full wave32;
the separate executable property reports 128 and is preserved as reported.
These static observations do not establish occupancy, instruction stalls or
the cause of the measured slowdown.

## Measurements

All 36 stages use actual imported W1/W2 weights and fixed, finite synthetic
activations at their target model shapes. C64/E2 covers blocks 5–8/62–65 with
103,680 rows; C128/E4 covers 9–14/56–61 with 25,920 rows; C256/E8 covers 15–22/48–55
with 6,480 rows and 6,528 allocated rows. These are not captured graph activations,
including for the first decoder blocks.

Each block uses five warmups per role, then three contiguous 30-frame pairs:
B30-C30, C30-B30, B30-C30. That retains 90 samples per role/block. Persistent
buffers, descriptors, command buffers and a fence are reused. ALL_COMMANDS
timestamps include the two baseline dispatches or one candidate dispatch and
their compute barriers. Uploads, readback, statistics capture and CPU work are
outside measured spans. Fixed inputs and weights are shared between roles;
complete guarded outputs match the strict canonical result before and after
timing in all 72 checks. Pair order and variation are retained in the record.

| Family | Operators | Sum baseline medians, ms | Sum candidate medians, ms | Per-operator regression |
| --- | ---: | ---: | ---: | ---: |
| C64 / E2 | 8 | 2.03210 | 5.36150 | 156.07–171.20% |
| C128 / E4 | 12 | 2.13610 | 4.65314 | 116.01–120.07% |
| C256 / E8 | 16 | 1.97718 | 3.72550 | 87.47–89.28% |

The isolated sums are **6.14538→13.74014 ms**, a **123.58% regression**.
They are sums of separately measured operator medians, not a measured complete
inference frame. The earlier instrumented profile's 6.579260 ms W1+W2 cost bound
is separate evidence and is not interchangeable with these ordinary operator
timings. No operator meets the required 5% improvement gate.

## Validation limits and next hypothesis

The two fresh synthetic suites pass 903 full-allocation pairs/282,634,752 bytes.
The model-weight screen passes 252 pairs/2,512,484,352 bytes across all 36 blocks
and 72 matrices, plus 396 logical canary checks. Production/capture E4 twins,
hidden E4 and separately compiled activated-half/W2-half diagnostics are checked.
F168/F32 diagnostics do not expose the production F152/F16 pre-E4 bits.
Independent audits bind the source, compiled modules, model hashes, root exits,
saved byte results and all scalar timing statistics. The scalar record keeps
those identities; prototype source, raw tensors, weights and captures stay
private.

The successful runs have validation layers disabled. The explicit
VK_LAYER_KHRONOS_validation attempt exits before device creation because the
layer is absent. Zero reported validation errors without that layer does not
establish layer coverage.

The performance rejection stops this candidate before actual captured-input
replay, all 75 checkpoints/head, composed-frame quality, history, native bridge
or lifecycle tests, complete inference timing and game FPS. No new broad motion,
HDR, 60 FPS or original NVIDIA qualification is implied.

A separate **source-only** hypothesis keeps the logical 64-row tile but streams
two 32-row W1→W2 subpasses with one F32 accumulator per wave and planned 8 KiB
scratch/hidden storage. It must carry each original N16/logical-wave publication
state across row 32 while retaining K16, original SiLU and software E4. Duplicated
A loads and extra staging barriers are tradeoffs. It has no compilation, GPU,
preservation, occupancy or performance qualification and inherits none from this
rejected prototype.

The core's [MIT license](../LICENSE) and adapter's
[GPL notice](../NOTICE) remain unchanged. NVIDIA DLLs, generated weights and game
assets are excluded from commits and release packages.
