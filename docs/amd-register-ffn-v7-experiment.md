# Synchronized native-half register FFN experiment

The private v7 register FFN passes its bounded and actual-input byte checks, but is **36.81–44.14% slower** than the frozen two-dispatch Pair baseline on the seven ordinary C32 blocks. It is rejected for integration. Alpha 6, the qualified cache, default selections and NR's disabled default remain unchanged.

The [scalar evidence](performance/c32-register-ffn-v7-rx9070xt-20261003.json) and [complete interleaved timing record](performance/c32-register-ffn-v7-measurements/ordinary-target-interleaved.json) identify the tested binaries, modules, inputs, driver and each paired result. The earlier [v5 record](performance/c32-register-ffn-v5-rx9070xt-20261003.json) remains unchanged.

V7 replaces only the K16 accumulator's software FP16 publication with the existing native RTE helper, and adds an explicit barrier before overwriting shared expansion-seed storage. It retains software residual publication, corrected SiLU, the frozen driver's Pair E4 publication ancestry, row gathering and the register transpose. The terminal E4 behavior is an installed-driver compatibility result, with no arbitrary-input or portable-reference claim.

Both routes use K32 expansion to 128 channels and K128 contraction to 32 channels, one batch, no partition, and an E4 residual. The target is 1707×960 padded to 1728×960; each ordinary block has 414,720 rows. Inputs are the same seven generated-model graph activations, with QKV normalization off. They are not gameplay captures.

The small run passes 315 comparisons across 45 fixtures; the target run passes 329 across 47, including those same small fixtures. An independent CPU audit reopens 1,288 saved buffers, compares 590,946,304 bytes across the two runs and checks 8,144,384 canary bytes. The separate actual-input replay passes 49 comparisons across seven fixtures and 2,043,740,160 bytes. Its weight matrices, residual scales and fixture identities are independently reconstructed. These aligned actual inputs have no allocation padding; tails and padding are covered by the bounded suite. An extra F40 activated-F16 trace does not establish the original F24 pre-E4 half bits.

Timing uses five warmups per route, followed by three interleaved pairs of 30 measured frames per route and fixture. Commands, descriptors and buffers are persistent. Image uploads, readback and pipeline statistics are excluded from measured frames. Full F16/E4 output hashes before and after timing match the saved actual replay. Completion uses `vkQueueWaitIdle` per submission; GPU spans include barriers and exclude CPU submission and the D3D12 bridge.

| Block | Pair median, ms | V7 median, ms | Slowdown |
|---|---:|---:|---:|
| 1 | 0.51148 | 0.73726 | 44.14% |
| 2 | 0.51210 | 0.71720 | 40.05% |
| 3 | 0.52464 | 0.73660 | 40.40% |
| 4 | 0.52042 | 0.72654 | 39.61% |
| 67 | 0.53424 | 0.73336 | 37.27% |
| 68 | 0.53312 | 0.73018 | 36.96% |
| 69 | 0.53400 | 0.73056 | 36.81% |

Every one of the 21 paired runs is slower, by 33.08–46.58%. The sum of isolated medians rises from 3.67000 to 5.11170 ms. This sum is an operator estimate; it does not measure complete inference or game frame time. The baseline has an extra middle timestamp, and the B/C, C/B, B/C ordering is unbalanced. All samples and variation remain in the record; no clock or thermal cause was measured. The separate v5 and v7 protocols cannot establish a paired causal improvement from native publication.

Installed-driver reports allocate 77 VGPRs/64 SGPRs to production and 82/74 to the capture twin, with 1,536 bytes of LDS and zero scratch. They contain four static FP8 WMMA instructions and 32 static transport shuffles, with no terminal FP8 hardware downconversion or MODE setter. These reports were collected separately from timing. Offline reports and static instruction counts do not prove runtime occupancy or instruction throughput.

The probe remains private and is excluded from release binaries. No full-graph fusion, temporal, lifecycle, visual-quality or gameplay gate was run for this rejected route. The 8 ms NR-plus-bridge budget and 16.67 ms complete-frame/60 real FPS target remain unmet.
