# Fusion and normalization screens after alpha 5

Independently audited scalar evidence. No release/default/cache change. Pair/Arena is the alpha 5 diagnostic baseline; the shipped auto/default cache retains Direct-RTE/Register-RTE.

The preserving ordinary C32 FFN fusion passed bounded byte checks but cost roughly twice as much as the decomposed Pair route. The transport-only QK shuffle changed ordinary network median time by 0.033%, below the 5% promotion gate. The paired QK half-arithmetic version failed a mixed-pattern comparison.

| Experiment | Result | Decision |
|---|---|---|
| Ordinary C32 FFN compatibility fusion v2 | 301 operator comparisons and 28 actual-target pre/post output twins matched; seven isolated block medians sum to 3.60064 ms baseline versus 7.21894 ms fused | Reject performance; no graph integration |
| Packed QK half arithmetic | One of 95 bounded fixtures differed: 1,216 K bytes across 38 rows | Reject numerical comparison |
| QK shuffle transport only | Ordinary target network median 50.55104 → 50.53452 ms; 0.03268% improvement | Keep diagnostic; no promotion |

FFN measured each of the seven ordinary blocks using its imported weights and matching full-target model-generated activation/residual input (414,720 rows, C32). Five warmups preceded three interleaved baseline/candidate pairs of 30 frames per route. Persistent preallocated commands, pipelines and buffers had no image upload, output readback or capture flags during measurement. GPU spans include compute barriers; the two-dispatch baseline has a middle timestamp as well as start/end. These are instrumented operator times and exclude CPU submission, full inference, D3D12 bridge, FSR and game FPS.

| Block | Pair expansion + contraction (ms) | Fused (ms) | Change |
|---|---:|---:|---:|
| 1 | 0.50244 | 1.03976 | 106.94% slower |
| 2 | 0.50312 | 1.10068 | 118.77% slower |
| 3 | 0.51762 | 1.03542 | 100.03% slower |
| 4 | 0.51518 | 1.04042 | 101.95% slower |
| 67 | 0.52072 | 0.99364 | 90.82% slower |
| 68 | 0.52152 | 1.00546 | 92.79% slower |
| 69 | 0.52004 | 1.00356 | 92.98% slower |

The 3.61830 ms sum of additional operator cost is an isolated-median estimate, not a complete-graph timing result. No block passed the 5% operator improvement gate. Explicit upload/readback dependencies were present in the final v3b run. The CPU audit reopened all 301 raw output pairs (587,026,432 bytes), checked 602 canary buffers, and recomputed 560 timing-statistic fields. Passing timed final-twin buffers were compared before/after the protocol but were not saved; their checks are bound through the report and byte-comparison source.

This FFN candidate preserves the frozen RX 9070 XT / AMD 26.9.1 Pair publication behavior only within the tested scope. Its software E4 publisher reproduces observed wave conversion ancestry, including first overflow codes 7f/ff. It is not the portable E4 saturation specification or an arbitrary-input equivalence proof. The separate native-clamp arithmetic experiment is excluded from these results.

The QK shuffle whole-network run used the alpha5 Pair/Arena Q32, N16, stage16, K16 route with all fusions off, at valid 1707×960 padded to 1728×960. Five warmups and three interleaved pairs of 30 frames produced 90 samples per role; image readback, dispatch instrumentation and pipeline capture were off. Baseline p95 was 50.73370 ms and candidate p95 50.76843 ms (0.06847% regression); p99 was 50.88334 versus 50.87656 ms. The 2% regression gate passed, but the 5% complete-inference improvement gate failed. Operator improvement was not separately qualified.

The original QK bounded/model diagnostics and ordinary timing use the frozen build from before the upload, fill and download visibility repair. Their recorded matches, failures and timings are historical observations of that build, not qualification or timing of the current source. The packed-half failure has installed-driver scheduling evidence involving FP16 overflow mode, but no forced-mode experiment establishes causality. This limits numerical claims and does not invalidate the readback-free ordinary timing protocol.

The record contains scalar timings and artifact hashes only. NVIDIA DLLs, extracted weights, activation/output buffers and game captures are excluded. There is no new game FPS, bridge, VRAM, motion/HDR quality or full-model FFN qualification. NR remains disabled by default and the 8 ms NR-plus-bridge / 16.67 ms complete-frame targets are not established by these experiments.

The [scalar record](performance/post-alpha5-fusion-normalization-rx9070xt-20261003.json) carries the measured samples, identities and scope. The [buffer visibility repair](amd-context-visibility.md) has separate fresh correctness evidence.
