# Expert W1–W2 M32 streaming screen

The private M32 streaming prototype is **rejected for performance**. All 36
model-weight operators pass the evaluated strict bytes, but pooled medians are
**42.44–68.01% slower**, and all 108 paired medians are **28.89–74.06% slower**.
No operator reaches the required 5% improvement gate. No shipping kernel,
default, qualified cache, ABI or release changes; NR remains disabled by default.

The qualified alpha 6 explicit Pair/Arena/K16 route with C32 QKV normalization
on remains **47.74616 ms median network inference** at 1707×960, padded to
1728×960. This experiment adds no complete-inference or game measurement. The
8 ms NR-plus-bridge and 16.67 ms complete-frame targets remain unmet.
See the [scalar record](performance/expert-w1-w2-m32-rx9070xt-20261004.json)
for all 6,480 samples, pooled median/p95/p99/variation and all three paired
statistics per operator. The [qualified QKV record](performance/qkv-normalize-rx9070xt-20261003.json)
retains the separate ordinary network measurements.

## Candidate and evaluated bytes

The baseline runs separate original repaired Pair W1 expansion and W2
contraction with K16/N16/stage16. The candidate retains a logical 64-row,
128-thread workgroup but streams two 32-row W1→W2 subpasses using one F32
accumulator per wave and 8 KiB of shared scratch/hidden storage. Each original
N16/logical-wave software E4 publication state carries across row 32. Native
FP8/F32 matrix operations, ordered K16 half publication, original corrected
mixed3 SiLU and opaque raw-positive-zero seed remain. W3, residual and the rest
of the graph are outside this operator screen. This measures the previously
source-only M32 hypothesis; the [earlier 16 KiB fusion](amd-expert-w1-w2-fusion-screen.md)
has separate frozen evidence.

The fresh bounded/target suites pass **903 full-allocation pairs across 129
fixtures / 282,634,752 compared bytes**, with 1,419 logical canary checks. A
separate 15-fixture row-boundary supplement passes **105 pairs / 3,141,120 bytes**
and 165 logical canary checks, including rows 31/32/33 and zero/raw-NaN prefixes.
These comparisons do not infer an unobserved production output class.

The model-weight suite covers all **36 expert blocks and 72 W1/W2 matrices**:
C64/E2 blocks 5–8/62–65 at 103,680 rows; C128/E4 blocks 9–14/56–61 at 25,920 rows;
C256/E8 blocks 15–22/48–55 at 6,480 rows, padded to 6,528. Activations are fixed
finite synthetic values, including allocated padding, rather than captured
graph inputs. All **252 full-allocation pairs / 2,512,484,352 compared bytes**
match, with 396 logical canary checks. Production/capture E4 twins, hidden E4
and separate activated-half/W2-half diagnostics are compared. F168/F32
diagnostics do not expose original F152/F16 production pre-E4 bits. Fresh
source, module, executable, root-exit and output audits are bound in the record;
no old numerical qualification is inherited.

## Ordinary operator timing

Each block uses five warmups per role, followed by three contiguous 30-frame
pairs: B30-C30, C30-B30, B30-C30. All 90 samples per role/block are retained.
Persistent buffers, descriptors, command buffers and fences are reused.
ALL_COMMANDS timestamps cover two baseline dispatches or one candidate dispatch
and compute barriers. CPU work, uploads, readback and executable-statistics
capture are outside these spans. All 72 complete guarded pre/post outputs match
the strict canonical result. No runs are discarded; pair order and variation
remain visible in the scalar record.

| Family | Operators | Sum baseline medians, ms | Sum candidate medians, ms | Per-operator regression |
| --- | ---: | ---: | ---: | ---: |
| C64 / E2 | 8 | 2.04348 | 3.35972 | 50.08–68.01% |
| C128 / E4 | 12 | 2.16808 | 3.12162 | 42.44–44.98% |
| C256 / E8 | 16 | 2.00994 | 3.18024 | 56.55–59.49% |

The isolated median sums are **6.22150→9.66158 ms**, a **55.29% regression**.
They sum separately timed operators and are not a measured inference frame.
Neither this sum nor a smaller shared-memory allocation establishes a network
or game improvement.

## Installed resources and stopping point

Installed-driver production VGPR/SGPR counts are **78/74, 79/74 and 71/68** for
C64/C128/C256. All candidate modules use **8 KiB LDS**, with zero scratch
allocation/static scratch instructions, native FP8/F32 WMMA and no hardware E4
conversion or MODE setter. Explicit installed spill counters are unavailable.
The pipeline requests full wave32; the separate raw executable property reports
128 and does not independently establish installed wave width. Offline and
installed resources are distinct observations. They do not establish dynamic
occupancy, stalls or the cause of the measured regressions.

Successful suites and timing runs report **validation layers disabled** and
zero validation errors; this does not establish layer validation coverage.
The performance rejection stops before actual captured graph inputs, all 75
checkpoints/head, composed-frame quality, histories, bridge/lifecycle, complete
inference timing and game FPS. The evaluated bytes establish only the bounded
AMD baseline correspondence; arbitrary-input preservation and original NVIDIA
parity remain unqualified. There is no alpha 7 package or default/cache change
from this screen.

The core [MIT license](../LICENSE) and adapter [GPL notice](../NOTICE) remain
unchanged. Prototype source/binaries, raw tensors, generated weights, NVIDIA
DLLs, images, logs/ISA and game captures remain excluded from publication.
