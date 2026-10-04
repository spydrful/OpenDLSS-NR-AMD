# C32 QKV and normalization fusion

The RX 9070 XT candidate reduces target network inference from **50.41260 to 47.74616 ms median (5.2892%)** with AMD 26.9.1. The baseline and candidate use the same frozen final-v3 executable, with the explicit QKV normalization selector set to `off` and `c32`. These are network timings; the D3D12 bridge, FSR, presentation and Cyberpunk frame time are excluded. The result remains above the **8 ms NR plus bridge** target.

| Ordinary network timing | Baseline | C32 fusion | Median gain |
|---|---:|---:|---:|
| 1707×960, padded 1728×960 | 50.41260 ms | 47.74616 ms | 5.2892% |
| 1920×1080, padded 1920×1152 | 67.99894 ms | 64.42920 ms | 5.2497% |

Target p95 improves from 50.63238 to 48.02880 ms and p99 from 50.78920 to 48.68730 ms. At 1080p, p95 improves from 68.30533 to 64.76994 ms and p99 from 68.32918 to 64.84796 ms. Each geometry uses five warmup frames per run and three interleaved baseline/candidate pairs of 30 measured frames, ordered B/C, C/B, B/C. Image readback, raw captures, pipeline statistics and dispatch instrumentation are disabled during these ordinary timing runs. Target pair gains are 5.0952%, 5.2969% and 5.3603%; 1080p pair gains are 5.2719%, 5.4461% and 5.0896%. There is no quality claim at the separately timed 1080p geometry.

The kernel fuses C32 QKV GEMM and the existing window normalization through shared memory. It retains ordered K16 FP16 publication, the scalar normalization reduction and learned scale. Each 256-thread workgroup produces 16×96 outputs through six active wave32 matrix tiles and 9 KiB declared shared memory. It covers the supported imported model's blocks 0–4 and 66–70. The required independent policy is optimized Pair/Arena, Q32, K16, N16 and stage16, with the other fusion and hardware-publication options off and no tuning file.

Select the route explicitly with `--amd-qkv-normalize c32`, or set `DLSS5VK_AMD_QKV_NORMALIZE=c32` before game-session creation. The default is `off`. Unsupported devices, incompatible forced geometry or policy, and any tuning file fail visibly. The process environment is read once at session creation. Existing auto/default cache selections and all previous release assets remain unchanged. Public tuning export for this route is blocked pending a dedicated fused-operator proof schema.

The all10 operator report passes 450 byte comparisons across 150 fixtures, including ten actual target network activations with locally imported weights. A separate persistent timing harness passes 60 pre/post comparisons; the reported output hashes match 30 independently reopened graph references. Its ten isolated operator medians sum to 5.81754 ms before fusion and 3.08282 ms afterward. This sum is not an actual network frame. All 30 paired operator comparisons improve by 41.574–51.766%, although order-dependent variation remains and its cause is unresolved. Fixture sets overlap, and passing raw operator buffers are not saved.

The final-v3 production route reproduces the frozen AMD bytes at all 75 same-policy decomposed model checkpoints, nine block0 intermediates and the F32 head at 320×320. Target validation checks the saved production and captured F32 heads. All 92 saved correctness files, totaling 282,656,512 bytes, were independently reopened. Captures decompose the same selected arithmetic; separately recorded fused-production heads match them. This does not expose every fused-production boundary or establish all 75 target checkpoints.

Six synthetic SDR/HDR/reset/temporal/camera-reset cases at 320×320 pass 18 strict head, composed-output and history checks against the prior AMD Pair/Arena policy. Their finite, unclamped scene-linear RGB is exact with a fixed data range of 1.0: SSIM is 1.0 and PSNR is infinite. Existing absolute-reference highlight failures are unchanged; preservation does not establish complete release-quality acceptance or HDR display validation. The existing eight recorded SDR game frames also match in both identical-history and independently evolved own-history replay. Of 80 file checks totaling 2,107,883,520 bytes, **48 check freshly computed replay head/history/composed outputs and 32 check copied original-runtime anchors**. All 16 freshly composed RGB frames are exact. Evolved histories start from a reset and match the corresponding prior replay; they do not claim parity with the original captured runtime histories. No new game sequence or broad motion, faces, ghosting or flicker review is claimed.

Native DLL lifecycle checks pass at 320×320 and 1707×960 with eight requested-geometry frames submitted sequentially. A separate source-included runtime pool verifies eight distinct prepared live slots before submission, plus cancellation, gap/reset, resize, drain and shutdown. This establishes slot ownership without claiming parallel inference. Runtime output comparisons run inside the harness and are documented by hash-bound logs; those buffers are not saved for the CPU audit. The legacy preserving suite separately passes 934 comparisons across 724 operators and 15,264,768 bytes with QKV normalization **off**; it does not exercise the fused C32 operator.

A separate instrumented target profile replaces 20 baseline QKV/normalization dispatches with ten fused dispatches, reducing 513 dispatches to 503. All 493 remaining dispatch metadata entries are unchanged. Thirty frame-aligned dispatch sums have median 50.21182 ms before fusion and 47.62824 ms afterward. Family medians below come from each family's sum within each actual frame; their medians should not be added to reconstruct a frame.

| Instrumented family | Baseline median | C32 median |
|---|---:|---:|
| FP8 GEMM | 32.12316 ms | 30.09466 ms |
| Window normalization | 4.84638 ms | 1.05316 ms |
| Fused QKV normalization | — | 3.12548 ms |
| Window attention | 6.48616 ms | 6.59034 ms |
| F16 GEMM | 3.31314 ms | 3.33166 ms |
| Global attention | 2.33808 ms | 2.34084 ms |

The profile is separate from ordinary timing. It retains each dispatch's family, shape, flags, partition, variant, geometry and frame samples. Independently minimized dispatch spans remain separately labeled and are not measured frames. Installed-driver statistics for the new module report 32 VGPRs, 22 SGPRs, 9,216 bytes of LDS and zero scratch. Two FP8 WMMA instructions appear in the captured static ISA. These are static counts, not dynamic instruction delivery or proof of ordinary no-capture code identity. The raw executable property `subgroup256` does not override the explicitly required full subgroup size of 32.

The [scalar evidence](performance/qkv-normalize-rx9070xt-20261003.json) binds the frozen precommit source, build, model, driver, raw-check records and independent CPU audits. Its measurement catalog links 12 ordinary benchmark files, two instrumented profiles and a separately identified public-tool operator timing file with hashes. Historical private graph and production-v2 results retain separate identities.

The later [verified publication receipt](performance/alpha6-release-identities.json) binds the actual alpha 6 remote tag and published GitHub assets to source commit `8da4db5d24a904d0a5216a7c4a71ed18e61d2da6`. The audited package excludes NVIDIA DLLs, weights and captures; its clean corresponding-source build passes, and all 32 native plus two game SPVs exactly match both the frozen tested build and package. Executable/DLL byte reproducibility is unclaimed. The receipt preserves the original precommit evidence scope, the five earlier releases and the frozen alpha 6 package.

The [standalone diagnostic script](../scripts/probe_amd_c32_qkv_normalize.ps1) builds three source-only tools. Its fresh CPU build passes 69,862 probe checks, 69,904 timing checks and 69,863 capture checks; these share substantial coverage and are not cumulative. Twenty-four invalid-command guards pass without a Vulkan context. All 32 generated application modules match the frozen production modules. Catalog files stay local to their directory; imported weights, ten target activation tensors and normalized/raw-QKV references are hash-bound. Timing requires the same build's passed 450-check report and recreates all fixture identities. Device/driver and capture-executable ancestry are enforced. The transparent selective graph copy changes only which tensors are saved and compares its production head with the ordinary graph. The diagnostic raw-capture shader is excluded from shipping game pipelines.

Fresh GPU reproduction through this corrected public source port passes capture, strict checks and operator timing. The CPU audit independently reopens thirty graph-reference tensors totaling 2,123,366,400 bytes, verifies the anchor features/head and recreates thirty actual-weight/input fixture identities. The strict report passes 150 fixtures and 450 comparisons totaling 2,554,011,648 bytes; its twenty reported target-output hashes match the saved graph references. The 60 pre/post timing comparisons also match those references. Passing operator output buffers are not saved. Selective-versus-production head equality is reported inside the capture harness; the selective head is not separately saved for reopening.

These fresh public-tool operator timings are distinct from the earlier private-harness measurements. All 1,800 samples and 720 statistic fields reproduce independently. Ten isolated operator medians sum to **5.81138 → 3.04080 ms**; this is not a network frame. Pooled operator gains are 43.7001–51.6911%, and all thirty individual pairs exceed 5%. Eight ordinary-block candidate third-run medians are 9.94–12.75% below their first runs; the clock, thermal or driver cause remains unmeasured. Every run is retained, and the B/C, C/B, B/C order remains unbalanced. Neither workflow establishes complete game FPS or default promotion.

Run the following from a source checkout with the repository's Windows build prerequisites and a locally imported model. Every output directory must be new. `Build` performs CPU compilation/checks; each GPU mode requires `-Run` explicitly.

```powershell
$nrDiagBuild = 'build/c32-qkv-local-build'
$nrLocalModel = 'models/imported/open-nr'
./scripts/probe_amd_c32_qkv_normalize.ps1 -Mode Build -OutputDirectory $nrDiagBuild
./scripts/probe_amd_c32_qkv_normalize.ps1 -Mode Bounded -Run -BuildDirectory $nrDiagBuild -OutputDirectory 'build/c32-qkv-bounded'
./scripts/probe_amd_c32_qkv_normalize.ps1 -Mode Capture -Run -BuildDirectory $nrDiagBuild -Model $nrLocalModel -OutputDirectory 'build/c32-qkv-capture'
$nrLocalInputs = 'build/c32-qkv-capture/result/qkv-inputs.json'
./scripts/probe_amd_c32_qkv_normalize.ps1 -Mode Check -Run -BuildDirectory $nrDiagBuild -Model $nrLocalModel -ActualInputs $nrLocalInputs -OutputDirectory 'build/c32-qkv-check'
./scripts/probe_amd_c32_qkv_normalize.ps1 -Mode Timing -Run -BuildDirectory $nrDiagBuild -Model $nrLocalModel -ActualInputs $nrLocalInputs -StrictReport 'build/c32-qkv-check/result/manifest.json' -OutputDirectory 'build/c32-qkv-timing'
```

Capture generates thirty local network tensors totaling about 2.12 GB, plus head/features and graph GPU allocations. These stay under ignored `build/`; they are not packaged. These standalone reports do not satisfy the public auto-tuner's fused-operator proof schema and do not establish game performance.

NR remains disabled by default. No arbitrary-input, cross-driver, NVIDIA parity, complete Cyberpunk FPS, ten-minute gameplay, 60 FPS or default-promotion qualification is claimed. NVIDIA DLLs, weights, game captures, raw activations and ISA text are excluded from the published evidence.
