# Two-expert shared-A performance screen

The private C64 shared-A candidate is byte equal on its recorded fixtures,
but all eight full-shape model-W1 operators are slower. Pooled medians regress
16.871-17.432%; every one of the 24 interleaved pairs regresses 16.634-17.575%.
It fails the 5% operator improvement gate. There is no graph followup, promotion,
shipping selector, default/cache change or new release.

The [scalar evidence](performance/expert64-a-reuse-rx9070xt-20261004.json)
binds the original source, successful bounded tests, installed-driver reports,
model8 strict replay, all timing samples and independent audits. The
[timing JSON](performance/expert64-a-reuse-measurements/model8-synthetic-interleaved.json)
is an exact scalar copy. Weights, raw buffers, ISA and private binaries remain
outside commits and release packages.

The candidate targets FLAGS152/K64/N128/Nmatrix128/P0/B2 with N16/stage16 and
ordered native K16 half publication. It uses 256 threads as two independent
original four-wave32 expert cohorts. A 4096-byte raw E4 input tile is shared
between experts; each cohort retains its own 4096-byte F32 matrix scratch arena,
original weight addressing, corrected mixed3 SiLU and original eight scalar
terminal publication visits. Raw positive-zero seed, supported device/driver,
matrix/float controls, full subgroup, memory, offset and safeY/Z grid guards
are required before pipelines. The broader K128/K256 expert families remain
outside this probe.

The committed alpha6 profile motivated this scope: these eight K64/B2
dispatches have a 1.70620 ms median of their within-frame summed instrumented
spans. That profile is separate from ordinary timing and uses different input
values. It is not compared directly with the new synthetic-input medians.

The fixed RX 9070 XT / AMD 26.9.1 bounded suite passes 59 small and 61 target
full allocation pairs, 56,360,960 compared bytes and 3,267,584 checked canary
bytes. It covers raw E4 codes, signed zeros, nonfinite/overflow cases,
offsets, broadcasts, tails, overdispatch and imported model W1s. No edges
are waived. First-NaN ancestry is explicitly CPU/source SiLU(-Inf) reasoning;
the observed terminal E4 result is cleaned +0. A zero E4 byte does not reveal
its pre-E4 NaN class.

The separate model8 run uses all eight imported C64 W1s at R103680 with
deterministic synthetic finiteE4 activations. These are not captured graph
inputs. The independent audit repacks every W1, regenerates every activation
array and fixture identity, then reopens 16 saved buffers. All eight complete
output pairs match: 252,149,760 compared bytes. Strict rows include 16 prefix
and 32 suffix canary bytes around 256 active E4 values. There are 39,813,120
comparable padding bytes; checking both raw roles examines 79,626,240 canary
bytes. Actual target rows are aligned; padded tails are covered separately
by the bounded suite.

Timing reopens the same executable's strict proof before Vulkan setup,
removes only output row padding and checks its 256-byte active rows against
the strict canonical E4 hashes before and after measurement. All 16 checks
pass. Inputs and weights are uploaded before timing. Persistent command
buffers, descriptors, queries and a reusable fence measure ALL_COMMANDS
dispatch spans including the compute barrier. Readback, image upload,
pipeline statistics, CPU submission/wait and bridge work are outside those
spans. There are five warmups, then three interleaved pairs of 30 measured
frames per route, giving 90 samples per route/operator. All 1,440 samples,
pooled median/p95/p99/variation and all 24 pair summaries are retained.

| Block | Original Pair median(ms) | Shared-A median(ms) | Regression |
| --- | ---: | ---: | ---: |
| 5 | 0.20666 | 0.24186 | 17.033% |
| 6 | 0.20826 | 0.24370 | 17.017% |
| 7 | 0.20876 | 0.24398 | 16.871% |
| 8 | 0.20798 | 0.24376 | 17.204% |
| 62 | 0.20636 | 0.24218 | 17.358% |
| 63 | 0.20722 | 0.24268 | 17.112% |
| 64 | 0.20670 | 0.24206 | 17.107% |
| 65 | 0.20720 | 0.24332 | 17.432% |

The sum of isolated operator medians rises from 1.65914 to 1.94354 ms.
This is not complete inference latency. Both routes and their paired runs
vary; the unbalanced B/C,C/B,B/C ordering is disclosed and the cause of
variation was not measured. No result is selected or discarded.

Installed driver reports show 36/32 VGPR/SGPR and 4096 bytes LDS for the
original Pair, versus 26/30 and 12288 bytes for shared-A; both have zero
scratch and four static FP8 WMMA instructions. Each has 35 F32-to-F16 and 36
F16-to-F32 static conversions, 71 total. The lower register count did not
produce a measured win. LDS is a per-workgroup allocation, not proof of
additional resident workgroups or occupancy. Raw executable subgroup
properties 128/256 remain distinct from the required matrix wave32.

The installed terminal converter precedes the MODE setter in both modules;
offline LLPC lowering has the opposite ordering. Raw byte tests bind the
installed driver and exact modules. They do not establish portable numerical
equivalence or dynamic instruction delivery. Validation layers are recorded
disabled with zero errors, so this is not validation-layer coverage.

There is no complete graph, composed RGB quality, temporal sequence, bridge
timing or game FPS claim. Current shipping kernels, exact reference, defaults,
qualified cache and alpha releases remain unchanged. Core MIT notices and
the existing GPL adapter's corresponding source remain unchanged.
