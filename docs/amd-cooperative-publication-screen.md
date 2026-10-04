# Cooperative matrix publication screen

The private cooperative-constructor publication candidate was slower on the RX
9070 XT with AMD 26.9.1: all seven isolated operator medians regressed by
3.264-3.979%, and all 21 paired medians regressed by
3.045-4.132%. It was rejected at the operator performance gate.
The candidate is not distributed or integrated; alpha.6 and the default policies
are unchanged. NR remains disabled by default, and the qualified target inference
median remains 47.74616 ms against the unmet 8 ms NR-plus-bridge budget.

This isolates ordinary C32 SiLU expansion in blocks 1-4 and 67-69, at 414720
rows/K32/N128/P0/B1, using baseline-generated graph activations and imported W1
weights. The source geometry was 1707x960, padded to 1728x960. Original FP8 operands
and ordered K16 FP32 matrix operations remain. The candidate replaces each K16
scalar half publication with same-use/scope 16x16 F32-to-F16-to-F32 matrix
constructors, restoring original FP32 Inf/NaN sign and payload bits by integer
masking. IEEE finite overflow to infinity, corrected mixed3 SiLU, terminal E4
publication, opaque +0 seed and guard-false fallback source remain unchanged.
This is a preserving hypothesis with bounded checks, not arbitrary-input or
original NVIDIA parity qualification.

| Block | Original median ms | Constructor median ms | Slower |
| --- | ---: | ---: | ---: |
| 1 | 0.30868 | 0.31972 | 3.577% |
| 2 | 0.34228 | 0.35590 | 3.979% |
| 3 | 0.36088 | 0.37402 | 3.641% |
| 4 | 0.35892 | 0.37256 | 3.800% |
| 67 | 0.37218 | 0.38516 | 3.488% |
| 68 | 0.37524 | 0.38872 | 3.592% |
| 69 | 0.37740 | 0.38972 | 3.264% |

Each route received five warmups and three interleaved pairs of 30 measured
samples, ordered B/C, C/B, B/C: 1260 total samples. Persistent buffers, descriptors,
command buffers and fence were reused. ALL_COMMANDS GPU timestamps include the
operator and its compute barriers; they exclude upload/readback, CPU submission
and wait, the bridge and other model operations. No pipeline capture or statistics
were enabled during timing. Vulkan validation layers were disabled and zero validation errors were
reported. The [scalar record](performance/cooperative-publication-rx9070xt-20261004.json)
retains all samples and all 21 pairs, including median, p95, p99 and population
standard deviation/mean variation. Some tail percentiles improved; both routes
drifted for some blocks, and the paired order is unbalanced. The cause was not
measured and no runs were discarded. The isolated median sum, 2.49558 ->
2.58580 ms, is not a complete inference or frame time.

Fresh bounded tests passed 288 comparisons across 48 small fixtures and 300
across 50 target fixtures. Seven actual graph-input operators then passed all
42 full-allocation comparisons (2786918400 compared bytes), followed by all 14
canonical pre/post timing checks. Coverage includes F24 E4 output, separate F40
activated-half/E4 diagnostics and FLAG16/P0 plus FLAG24/P32 fallbacks. F40 does not
observe F24's internal pre-E4 half values. Independent saved-byte audits are
hash-bound; the publication helper reads scalar evidence only.

Installed F24 resources changed from 33 to 31 VGPRs and 29 to 33 SGPRs, with 4096
bytes LDS and zero scratch for both. Both report two static FP8/F32 matrix
instructions. These static resource changes do not explain or prove a speed gain.
The raw executable subgroup property of 128 is recorded separately from the
required subgroup size 32 and local workgroup size 128. Driver statistics were
collected separately from ordinary timing.

No complete candidate graph, checkpoint/head/history/lifecycle qualification,
composed RGB quality result, new network latency, bridge measurement or game FPS
claim follows this rejected screen. The exact reference, cache, ABI and release
are unchanged. Raw tensors, weights, game captures, proprietary DLLs, logs, ISA
text and PC addresses are excluded from publication.
