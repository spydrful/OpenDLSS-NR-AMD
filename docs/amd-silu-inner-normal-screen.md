# SiLU inner publication screen

The private SiLU inner software-publication candidate improved all seven
ordinary C32 expansion operator medians by 1.157-3.801% on the RX 9070 XT with
AMD 26.9.1. All 21 paired medians improved by 1.850-4.004%. Every result remained
below the required 5% operator gate, so the candidate was rejected for promotion.
Alpha.6 and its defaults remain unchanged, NR stays disabled by default, and the
current target inference median remains 47.74616 ms against the unmet 8 ms
NR-plus-bridge budget.

The candidate changes only the existing SiLU inner software half publication.
The finite F32 result of its unchanged bounded FMA lies in the inclusive range
0.2236328125-0.447265625. Integer round-to-nearest-even on that positive-normal
grid removes unreachable overflow/subnormal paths, while restoring complete
original F32 nonfinite sign and payload bits. Ordered K16 FP8/FP32 matrix
operations, their native half publications, other mixed3 SiLU boundaries,
original terminal E4 publication and opaque +0 seed remain unchanged. This
screen covers blocks 1-4 and 67-69, R414720/K32/N128/P0/B1/F24, using
baseline-generated graph activations and imported W1 weights from 1707x960 inputs
padded to 1728x960.

| Block | Original median ms | Inner-specialized median ms | Improvement |
| --- | ---: | ---: | ---: |
| 1 | 0.35356 | 0.34012 | 3.801% |
| 2 | 0.33198 | 0.32516 | 2.054% |
| 3 | 0.32664 | 0.32286 | 1.157% |
| 4 | 0.35498 | 0.34166 | 3.752% |
| 67 | 0.37380 | 0.35974 | 3.761% |
| 68 | 0.37554 | 0.36212 | 3.574% |
| 69 | 0.37506 | 0.36142 | 3.637% |

Each route received five warmups and three interleaved pairs of 30 samples,
ordered B/C, C/B, B/C: 1260 samples in total. Persistent buffers, commands,
descriptors and fence were reused. ALL_COMMANDS GPU timestamps include the
operator and compute barriers; uploads, readbacks, CPU submission and waits,
other operators and the bridge are outside the span. Pipeline statistics,
capture and Vulkan validation layers were disabled; zero validation errors were
reported. The [scalar record](performance/silu-inner-normal-rx9070xt-20261004.json)
retains every sample and all 21 pairs, with median, p95, p99, population standard
deviation and variation. P95 worsened for blocks 1 and 69; those results and all
pair variation remain included. The isolated operator median sum,
2.49156 -> 2.41308 ms, is not a network or frame time.

Fresh bounded tests passed 48 small fixtures/288 comparisons and 50 target
fixtures/300 comparisons. A separate generated-number GPU oracle passed 11
cases covering 8,388,609 finite F32 patterns and all 16,777,216 signed nonfinite
bit patterns, plus tails and extra X/Y/Z workgroups. Independent reopening
verified all 33 saved arrays, 302,116,992 bytes across three whole comparisons
and 9,936 canary bytes across both GPU routes. This establishes only the direct
inner software boundary on the bound driver. It does not observe the calling
FMA or prove arbitrary matrix or model arithmetic. Inherited unused F16 mode
declarations remain, with no live F16 type; the production float16 closure was
not applied to that standalone oracle.

The accepted actual-input run passed all 42 full-allocation comparisons across
seven operators, independently reopening 84 outputs/2,786,918,400 compared
bytes/371,589,120 canary bytes, followed by 14 canonical pre/post timing checks.
Coverage includes F24 E4, separate F40 activated-half/E4 diagnostics, FLAG16/P0
no-SiLU control and FLAG24/P32 partitioned SiLU. F40 does not observe F24's
internal pre-E4 half values. A preliminary host argument failure was corrected
before the accepted run and contributes no numerical result; its receipt is
retained separately, including the corrected scope of its launcher flag.

Installed resources remain 33 VGPRs/29 SGPRs/4096 bytes LDS/zero scratch for the
original and candidate F24/F40 modules. Both have two static FP8/FP32 matrix
instructions and 19 narrowing plus 20 widening half conversions. Static EXEC
branch counts decrease 7 -> 5. The installed terminal FP8 converter precedes the
relevant MODE setter in both routes, retaining the driver's scoped ancestry.
The raw executable property 128 is separate from required subgroup 32 and local
128. These counts are static observations, not dynamic utilization or a measured
cause of timing changes; driver reports were collected separately from timing.

No full candidate graph, checkpoint/head/history/lifecycle, composed RGB quality,
new complete inference or bridge latency, or game FPS is qualified by this
screen. There is no original NVIDIA parity claim. The prototype is not
distributed or integrated. Raw arrays, weights, actual activations, images,
game captures, proprietary DLLs, private paths, logs, ISA and PC addresses are
excluded.
