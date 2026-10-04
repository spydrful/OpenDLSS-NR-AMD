# C32 expansion accumulation screen

The separate `c32-expansion-final` arithmetic experiment fails our initial quality gate and is rejected. SDR temporal falls below 40 dB PSNR; every HDR case fails PSNR, and HDR temporal also fails 0.99 SSIM. The existing Alpha 6 package, qualified cache and disabled NR default remain unchanged.

The [scalar evidence](performance/c32-expansion-final-rx9070xt-20261003.json) records the exact source, module, CLI, inputs and metrics. The private Pair module changes only K32/F24/P0/N128/Nmatrix128/B1 SiLU expansion: it performs the same two ordered K16 matrix operations and publishes FP16 after their combined accumulation. All other Pair shapes retain K16 publication. Residuals, SiLU, terminal E4, attention and the exact reference remain unchanged. This was an arithmetic experiment, with no preserving claim.

One native module differs from frozen Alpha 6. The CLI, 31 other native modules, both game modules and qualified cache bytes remain frozen; tuning, fusion and QKV normalization are off. The inherited CLI and generic quality-tool fields say `k16`. The hash-bound private wrapper and modified source define the actual guarded arithmetic policy.

Both routes execute the complete graph on the same six generated 320×320 SDR/HDR cases, using identical source color/motion, controls, preprocessed features and previous histories. An independent CPU audit reopens 96 raw/PFM/control files and recomputes PSNR and local Gaussian SSIM from unclamped scene-linear RGB with a fixed data range of 1.0. Fresh baseline head, composition and published-history bytes also match 18 historical preserving outputs. Every matched composed frame must pass PSNR ≥40 dB and SSIM ≥0.99.

| Case | PSNR, dB | SSIM | Gate |
|---|---:|---:|---|
| SDR reset | 49.6341 | 0.998091 | Pass |
| SDR temporal | 39.2821 | 0.992539 | Fail |
| SDR camera reset | 49.6341 | 0.998091 | Pass |
| HDR reset | 34.1380 | 0.992538 | Fail |
| HDR temporal | 33.6038 | 0.986134 | Fail |
| HDR camera reset | 34.1380 | 0.992538 | Fail |

Camera-reset cases reproduce reset outputs, so these rows do not represent six independent inputs. The temporal cases use fixed synthetic histories; independently evolved histories, natural SDR and target-resolution sequences were not tested. The comparison reference is the frozen AMD Pair/Arena output. It does not establish original NVIDIA or portable-reference parity.

No 75-checkpoint capture, production/capture-head qualification, timing, bridge/lifecycle, motion review or gameplay followup was run after rejection. Offline wave32 resources are compiler diagnostics and do not establish a performance improvement. This candidate is excluded from integration and releases; the 8 ms NR-plus-bridge budget and 16.67 ms complete-frame/60 real FPS target remain unmet.
