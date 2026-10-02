# RX 9070 XT performance implementation and results

This records the implementation following [the performance research](amd-performance-research.md), checked on October 2, 2026. The new preserving direct-GEMM route reduces target inference median from **119.143 to 81.049 ms**, a **31.97%** reduction against the already-qualified Q32/shared-GEMM anchor, in three interleaved benchmark pairs. The earlier legal compact Q64-to-Q32 change measured **141.343 to 122.331 ms / 13.45%**. The historical **206.167 to 120.271 ms / 41.7%** comparison used legacy attention exceeding this GPU's shared-memory limit. All remain far above the **8 ms NR budget** and the **16.67 ms complete-frame/60 FPS goal**. These timestamps exclude the game, Direct3D bridge, FSR and presentation.

The development device is RX 9070 XT, Vulkan 1.4.349, Adrenalin 26.9.1/LLPC. Evidence is specific to this device/driver/model/shader combination. RX 7000 support, NVIDIA-runtime parity and broad game image/temporal acceptance remain separate work. The existing ABI is version 1; NR remains disabled by default and requires explicit user opt-in.

The [GEMM continuation](amd-gemm-delivery.md) now adds distinct packed and direct
operand routes against the already-qualified Q32/shared-GEMM anchor. Clean
public-binary operators, all 75 checkpoints at 320×320 and target output pass
strict byte comparison. The independent FFN32-only and QKV32-only routes also
pass the 320 checkpoint/head comparisons. Ordinary timing passes the prescribed
protocol and improvement gates; native lifecycle, bounded captured-history replay
and automatic cache GPU selection also pass. Three warmed alpha 3 game benchmarks
per condition average **97.12269 FPS NR off / 10.69410 FPS NR on**; pooled complete
NR-on frame times are **93.495 / 95.0625 / 95.9785 ms median/P95/P99**. Separate
asynchronous runtime brackets have NR-plus-bridge medians **85.84388 / 85.94782 /
85.90282 ms**, without a per-present join. See the
[alpha 3 game record](performance/cyberpunk-alpha3-20261002.json). Broad quality
and active-gameplay gates remain open. These source changes and their
measurements are separate from the unchanged published alpha 2 assets.

## Implemented paths

| Area | Implementation | Status |
| --- | --- | --- |
| Compact 64-query window attention | [amd_window_optimized.comp](../shaders/amd_window_optimized.comp), [shared body](../shaders/amd_window_optimized_body.glsl) | Preserving K16 path; strict operators, 320 model and target head pass |
| Smaller query groups | [amd_window_small.comp](../shaders/amd_window_small.comp), 16 or 32 queries per workgroup, full 64-key K/V | Both operator suites pass; Q32 target head passes and full target benchmark protocol passes |
| GEMM tile/staging experiments | [amd_gemm_optimized.comp](../shaders/amd_gemm_optimized.comp), N16/N32/N64 and staged K16/K32/K64 | Nine preview configurations measured; N16/stage16 retained; wide N64/stage64 strict regression fails |
| Packed/direct GEMM operands | [amd_gemm_packed.comp](../shaders/amd_gemm_packed.comp), [amd_gemm_direct.comp](../shaders/amd_gemm_direct.comp), separate actual route identities | Guarded public direct route passes 660 operators / 862 checks, 320/target output, ordinary interleaved timing, native lifecycle, bounded replay and automatic selection |
| C32 FFN and QKV/normalization/attention fusion | [amd_ffn32.comp](../shaders/amd_ffn32.comp), [amd_qkv32.comp](../shaders/amd_qkv32.comp), independent route controls | Opt-in; logical publications preserved; independent direct-GEMM 320 model checks pass; measured historical combined route slower |
| Expert FFN and complete C32 body fusion | [amd_expert_ffn.comp](../shaders/amd_expert_ffn.comp), [amd_block32.comp](../shaders/amd_block32.comp) | Opt-in; 320 model/capture comparisons pass, target preview regresses |
| Changed accumulation schedules | Publication specialization 10 in GEMM/attention/fusion; [amd_global_matrix_optimized.comp](../shaders/amd_global_matrix_optimized.comp) | K32/final are explicitly experimental; no quality-qualified promotion |
| Identity-bound selection | [amd_config.h](../src/amd_config.h), [kernels.cpp](../src/kernels.cpp), [qualified fallback](../src/amd_qualified_fallback.h) | Auto can use a pinned compact fallback or qualified session tuning |
| Complete timing export | [main.cpp](../src/main.cpp), [tune_amd.py](../tools/tune_amd.py) | Chronological frame samples, every profiled dispatch, complete family series |
| Strict binary qualification | [amd_kernel_preservation.cpp](../tests/amd_kernel_preservation.cpp), [qualify_amd_model.py](../tools/qualify_amd_model.py) | Actual operator/model/head buffers compared; execution identities and inputs bound to measurements |
| Compiled resource inspection | [analyze_amd_shaders.ps1](../scripts/analyze_amd_shaders.ps1), [pinned RGA identity](../scripts/rga_tool_manifest.json), `shaderinfo` | Offline wave32 ISA and installed-driver statistics kept distinct |
| Bounded captured-frame replay | [capture_request.h](../game/capture_request.h), `tune_amd.py replay`, `compositecheck` | Eight-frame synthetic lifecycle replay and eight genuine target-resolution game frames pass identical/evolved numerical replay; broad scene coverage pending |

The original `amd_window.comp`, portable reference arithmetic and NVIDIA paths remain available. The new kernels do not use NVIDIA PTX or require a CUDA compatibility layer. Raw model tensors still use the existing verified loader and original graph layout. No weights or NVIDIA model DLL are included in this work.

## Arithmetic and ownership invariants

Two meanings of exact must remain distinct. The software `--backend reference` emulates the documented Ada F13/F24 schedule and agrees with the independent direct WGSL diagnostic on the recorded 320 input. The new **preserving AMD** kernels instead compare byte for byte with the existing **accelerated AMD baseline**. AMD FP32 matrix accumulation is not Ada F13 arithmetic; preserving an AMD result does not establish equality with NVIDIA's runtime or the software reference.

The default optimized arithmetic uses FP32 cooperative accumulation and an explicit half publication after every K16 product group. It retains residual seeding/scaling, split-K partition publications and reduction order, SiLU placement, and E4/F16 dual outputs. Exact F24 adapter/head operations remain unchanged.

Window attention retains the 8x8 window, 32-channel heads, natural Q ordering, physical K/V untile permutation, shifted/OOB zeros, absolute learned-prior indexing, half-rounded score/exponential publications, fixed softmax tree and final E4 publication. Eight lanes own a query and publish eight of its 64 weights each. The 128-thread workgroup computes 16 query rows per softmax batch, using fixed XOR gathers rather than a generic reassociated reduction. Each wave has a private 16x16 float epilogue tile; the implementation makes no assumption about a cooperative matrix fragment component's lane-to-coordinate mapping.

Q16/Q32 split the query dimension into additional workgroups while retaining every physical key and prior. They duplicate some K/V staging to reduce per-workgroup LDS. This tradeoff is measured rather than assumed to improve occupancy. Fusion keeps logical half/E4 boundaries even when their temporary values remain in LDS. `modelcheck --intermediates` forces the decomposed capture implementation so the production fused head can be checked against an independent recording of the same selected AMD arithmetic.

`--amd-stage-k 32` changes operand staging while `--amd-arithmetic k16` still publishes after every K16. It must not be confused with `--amd-arithmetic k32`, which changes the numerical schedule. K32 publishes every 32 products; `final` publishes at product/partition completion. Both retain required partition/end publications and need separate image/temporal qualification.

The measured legal-anchor CLI is `5812da825825f0b50df964509e627d9773072709e3838e22c2935dbddb618778`. [The final identity record](performance/final-rx9070xt-binary-identities.json) separately records the packaged CLI/runtime, actual MSVC/glslang binaries, source-input closure and build/validation logs. Final package identities must not be substituted into earlier timing or game records. The application audit closes loaded-byte identity races in both core and game shaders, obtains model identity from the parsed manifest snapshot, and rejects over-limit attention. Shader aggregates are unchanged across the recorded application revisions. Eight-frame native harness and target-resolution production replay checks have been recorded with their own executable identities; those checks do not constitute fresh ordinary game benchmarks or prove bit-for-bit binary reproduction.

## Compiled resources and the legacy LDS discrepancy

The installed Windows driver reports the following for `window_attend_optimized@sg32:10=16`, using the Q64 compact kernel:

| Reported resource | Q64 compact | Q32 small-query |
| --- | ---: | ---: |
| Workgroup invocations | 128 | 128 |
| Required subgroup size at pipeline creation | 32 | 32 |
| Used VGPRs | 31 | 31 |
| Used SGPRs | 31 | 30 |
| LDS bytes per workgroup | 22,528 | 15,360 |
| Scratch bytes | 0 | 0 |

Q64's declarations account for 6 KiB Q/K/V operands, 4 KiB quantized weights, 8 KiB half scores and 4 KiB per-wave float scratch. Q16 and Q32 declare 11,776 and 15,360 bytes respectively; the Q32 installed-driver LDS result above agrees with its declaration, while Q16's source size is not substituted for a measured resource report. The fused FFN, QKV and complete C32 shaders declare 19, 28.5 and 30 KiB respectively. Compiler-added LDS or spills can change those figures.

The driver's executable-property field labeled subgroup size returns **128** for these 128-invocation compute kernels. That value is not used as proof of wave width. Native matrix pipelines explicitly request subgroup32. The offline analysis workflow separately requests native wave32, checks the compiled ELF wavefront metadata and records FP8 WMMA instruction names. The genuine, partial RGP trace separately confirms wave32 and native FP8 WMMA in the inspected Q32 attention pipeline. Its truncated SQTT does not qualify dynamic occupancy or complete-network delivery. The final target Q32 offline analysis covers all 32 GEMM specializations and the one shared window specialization. Offline ISA does not establish that the installed-driver pipeline has identical code.

The preserved legacy attention pipeline reports **34,816 bytes LDS** while this device exposes a **32,768-byte compute shared-memory limit**. [The resource guard](../src/amd_window_resources.h) rejects an explicit AMD `baseline` request before shader modules are loaded on this device, and checks every AMD attention allocation before pipeline creation. There is no limit override. The original shader and historical frozen measurements remain retained; driver acceptance of an earlier over-limit pipeline did not establish legality. Compact kernels fit the declared budget, with actual installed-driver statistics used where available.

The distinction matters when interpreting historical comparisons below: some earlier baselines used retained legacy attention. The legal Q64-to-Q32 experiment uses `compact64`, with optimized shared GEMM/K16/N16/stage16/Q64 and all experiments off. New direct-GEMM comparisons use `qualified32`, retaining Q32 and comparing shared against direct operands. Historical results are not relabeled.

## Strict numerical evidence

| Check | Result and scope |
| --- | --- |
| Earlier legal Q64/Q32 operator preservation | 657 operators, 858 paired-buffer checks; all pass |
| Earlier legal Q64/Q32 model at 320x320 | All 75 E4 boundaries, F32 head and both production/decomposed proofs pass: 77 actual binary checks |
| Earlier legal Q64/Q32 target output | Valid 1707x960, padded 1728x960; all 6,635,520 F32 head values and 4,916,160 composed RGB values bit-exact; production/decomposed proofs pass |
| Historical legacy versus Q64/Q16/Q32 operators | Each compact variant passes the same 657/858 suite; historical legacy anchor |
| Historical legacy versus compact model/head | All 75 E4 boundaries and complete F32 head at 320; target head and capture/production equality pass |
| Opt-in fusion at 320 | All 75 boundaries and head agree, including decomposed capture versus production |
| Forced-fusion native interop at 320 | Eight-frame production run, queue ordering, recovery, continuation bindings, slot lifetimes and drain/resize checks pass |
| Bounded synthetic capture/replay | Eight frames; identical and independently evolved AMD-baseline/candidate histories both match exactly, with SSIM 1 for every frame |
| Wide N64/stage64 | Fails 160 of 858 checks; not preservation-qualified |
| Optional hardware half publication | Fails 780 of 858 checks; software half publication retained |

The earlier legal Q64/Q32 target artifact manifest contains two actual binary checks: anchor/candidate head and both production/decomposed-head proofs. These complement the 77 checks at 320. The target scene composition also agrees bit for bit. The operator suite covers N16/N32/N48/N64; K32/K64/K128/K256/K512; 1/63/64/73/129-row shapes; batches/broadcasts; offset and tail canaries; F16/E4 residuals and scales; split-K; SiLU; dual publications; 1/2/4-head windows at 8x8 and 11x13 with 0/4 shifts and nonconstant priors; zero-row normalization; exact adapter/head; and all 65,536 half patterns for E4 conversion. It compares actual anchor/candidate GPU bytes, including padding and signed zeros. A coarse numerical tolerance in the older `selftest` is not this preservation proof.

The wide-tile failure includes E4 publication witnesses changing a signed `7f` code to `7e`. These are rejected even when the displayed numerical error seems small. The hardware-publication switch replaces explicit software half rounding with a pack/unpack route; its failures prevent promotion. Existing hardware E4 helpers already present in the baseline are a separate mechanism.

[The hardware-publication scalar evidence](performance/amd-publication-rounding-rx9070xt-20261002.json) now records a specific offline mismatch: the packed route compiles to 16 static `v_cvt_pk_rtz_f16_f32` conversions following two K16 WMMA groups, while the software route retains integer round-to-nearest-even tie handling. The packed instruction forces round-toward-zero and ignores the rounding mode, so changing that mode alone cannot repair this lowering. A previously captured finite normal witness changes binary16 `BF60` (-1.84375) to `BF61` (-1.8447265625); the failures are not confined to subnormals. The rejected installed-driver variant's ISA has not been inspected, so attributing its failures to this same lowering remains an inference. A scalar conversion with verified round-to-nearest-even control is a future isolated probe, not a qualified replacement. Hardware publication remains off.

Model fixtures use identical deterministic F32 feature bytes, fixed-point proxy colors, 12-uniform noise, reset history and fixed controls. Their producer is local Vulkan AMD, not NVIDIA or a game capture. The qualification script verifies actual feature sizes/hashes, graph boundary shapes, both execution identities and selected policies against their own benchmark records. It requires `captureImplementation=decomposed`. One binary proof pair concatenates baseline and candidate production heads and compares them with both captured heads, so the downstream gate independently verifies both schedules without trusting a PASS summary.

These results prove preservation for the tested inputs, modes and geometries. They do not certify arbitrary pathological inputs, NVIDIA parity, scene-linear image thresholds or evolving game history.

The controlled bridge run forces all three fusion switches on with Q64/K16. Its bounded eight-frame capture verifies ordered identity/ancestry/exposure metadata, required binary hashes, original source and prior-history capture, reset preprocessing and actual D3D12 RGBA16F truncation publication. Capture submissions are excluded from ordinary timing summaries. Replaying the accelerated AMD baseline against that candidate gives zero RGB differences and SSIM 1 on all eight frames in both identical-history and independently evolved-history modes. These are synthetic 320x320 inputs from the native harness, explicitly marked `genuine_game_capture=false` and `performance_representative=false`. They establish the tested bridge/capture mechanics and preserving temporal sequence, not genuine game image acceptance. Local logs and reports are retained under ignored `build/performance/interop-fusion8.log`, `interop-capture8.log` and `interop-replay-eight/`.

## Target timing results

The earlier legal-attention ordinary benchmark used an otherwise idle RX 9070 XT, actual locally imported weights, valid 1707x960 and padded 1728x960. Three pairs ran in legal Q64-anchor/Q32-candidate order; every child performed five warmup submissions and 30 measured submissions. Each role therefore contributes 90 chronological GPU samples. Image readback and per-dispatch profile instrumentation were disabled. Both use optimized K16/N16/stage16 without fusion or hardware publication; only window query count changes.

| Earlier legal-attention comparison | Median | P95 | P99 | Mean | Coefficient of variation |
| --- | ---: | ---: | ---: | ---: | ---: |
| Compact Q64 anchor | 141.343 ms | 143.036 ms | 143.603 ms | 141.491 ms | 0.006464 |
| Compact Q32 candidate | **122.331 ms** | **123.263 ms** | **123.534 ms** | 122.407 ms | 0.004664 |

The median reduction is **13.45%**, and P95 falls **13.82%**. The complete paired protocol and network regression/promotion gates pass, with strict preservation verified separately. [The earlier attention scalar evidence](performance/legal-compact64-rx9070xt-20261002.json) binds these measurements to their actual policies and binary identities. Scene quality, visual review and complete game acceptance remain separate and unmet.

The following earlier comparisons used the original legacy attention as their anchor. That pipeline exceeds the current device's LDS limit and is now rejected. These results remain historical; they are not current legal-anchor measurements.

| Interleaved experiment | Baseline median | Candidate median | Baseline P95 | Candidate P95 | Median / P95 ratio |
| --- | ---: | ---: | ---: | ---: | --- |
| Historical legacy → Q64, N16/stage16, K16, no fusion | 206.092 ms | 139.354 ms | 209.642 ms | 142.224 ms | 0.676177 / 0.678417 |
| Historical legacy → Q32, N16/stage16, K16, no fusion | 206.167 ms | 120.271 ms | 210.130 ms | 121.013 ms | 0.583366 / 0.575897 |

The historical Q32 record has median reduction **41.66%** and P95 reduction **42.41%** against its matched legacy baseline. Its candidate mean is 120.323 ms, P99 121.548 ms and coefficient of variation 0.00402. Those older sample statistics are not replaced by the newer legal comparison. A combined profile/network assessment has profile times at its top level; use its nested ordinary-network evidence or the ordinary bench report for network timing.

The earlier separate three-pair legal Q64/Q32 **instrumented profile** records 513 dispatches and whole-frame medians of **140.031 / 121.062 ms**. The table below gives the median of each complete family's per-frame summed spans across its 90 samples. It does not sum minima or reconstruct a frame by adding independent family medians. These instrumented times are separate from the ordinary benchmark above.

| Profile family | Legal Q64 anchor median sum | Q32 candidate median sum |
| --- | ---: | ---: |
| FP8 GEMM | 86.964 ms | 89.324 ms |
| Window attention | 42.269 ms | 20.854 ms |
| Window normalization | 4.568 ms | 4.492 ms |
| Exact F16 GEMM | 3.040 ms | 3.073 ms |
| Global attention | 2.110 ms | 2.176 ms |
| Conversion | 0.307 ms | 0.305 ms |
| Downsampling | 0.271 ms | 0.270 ms |
| Post-blend | 0.311 ms | 0.311 ms |
| Upsample/residual | 0.109 ms | 0.108 ms |
| Global normalization | 0.047 ms | 0.048 ms |

Attention supplies the observed saving. FP8 GEMM accounts for approximately 74% of the candidate's profiled frame and is slightly slower than its matched Q64-anchor family; wider GEMM/staging is not presumed beneficial. These family spans localize the next work rather than replace ordinary inference samples. The earlier legacy/Q32 profile recorded 109.525 → 20.616 ms for attention and a 119.092 ms candidate whole frame; that remains historical profile evidence.

A separate 1920x1080 comparison, padded to 1920x1152, used five warmups and 30 Q32/N16/stage16/K16 submissions: median **161.709 ms**, P95 **162.547 ms**, P99 **162.672 ms**. This is one candidate-only network run, not three interleaved pairs, a target-resolution qualification or a 1080p game FPS result.

The earlier legal-attention evidence is under ignored `build/performance/legal-compact64-20261002/`; these private directories are not distributed repository links. Its ordinary interleaved manifest SHA-256 is `fc40642b481aaf3897dbac6495305672af6c2a7236f3a5c5cd24a4de2574dc15` and measured CLI SHA-256 is `5812da825825f0b50df964509e627d9773072709e3838e22c2935dbddb618778`. The Q32 selected shader aggregate is `31a0295666cde46ad5d15d190e3ceaced19421531b06dcb7a5282c04999b9348`; Q64 is `e71855a4554be2cfc62a36faa4de7522b052a04a5148a54c46ab45989b188621`. The legacy aggregate `360c9488cd87c7e1de22d6b56f051921e9d4408f2efbd70ad1d3b01084ad3dae` identifies retained historical code, not a legal selected attention pipeline. Earlier evidence remains under ignored `build/performance/optimized-n16-k16/bench/` and `build/performance/final-q32/bench/`, whose ordinary Q32 CLI was `e3a46b0d18313e4c183661b657ff709c57acf34510043282a8496f4d84069952`.

The identity key is device `1002:7550`, driver `AMD proprietary driver|26.9.1 (LLPC)|8389003`, and model manifest `163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e`. Publishing these hashes does not publish the model.

### Preview results and rejected promotion

The following medians are five-frame previews after five warmups. All nine GEMM rows use **preserving arithmetic K16**, 64-query compact attention and no fusion; the second column is staged K, not arithmetic policy. They are screening observations, not complete performance/numerical qualification.

| GEMM tile N | Staged K | Preview median |
| ---: | ---: | ---: |
| 16 | 16 | 141.337 ms |
| 16 | 32 | 145.139 ms |
| 16 | 64 | 156.736 ms |
| 32 | 16 | 145.314 ms |
| 32 | 32 | 160.849 ms |
| 32 | 64 | 179.942 ms |
| 64 | 16 | 223.403 ms |
| 64 | 32 | 237.255 ms |
| 64 | 64 | 306.634 ms; strict regression also fails |

Q16 and Q32 previews measured 120.623 and 118.229 ms respectively. The older complete Q32 benchmark measured 120.271 ms median; the earlier legal-anchor paired result is 122.331 ms. A `final` arithmetic preview measured 112.837 ms but is not preservation-qualified or scene-quality-qualified.

All fusion flags enabled together preserve the tested 320 model outputs, but the 320 production timing is approximately **41 ms versus 19 ms without fusion**, and the target preview is **213.801 ms versus approximately 139 ms** for the unfused Q64 compact route. Fusion remains opt-in. Removing dispatches/intermediate publications does not by itself establish a faster complete network; compiled resources, duplicated loading, serial phases and barriers need further localization.

## Measurement and promotion contract

`bench` and `profile` default to valid 1707x960, warmup 5 and frames 30. Their `OpenNR-amd-benchmark-v1` JSON records actual chronological `frame_ms`, valid/padded geometry, requested and actual selected policy, device/driver/model identity, selected/frozen shader hashes and whether profile instrumentation is active. `readback=false` means no image readback; timestamp result retrieval still occurs.

Profile exports every dispatch with family, variant, rows/N/K/batches/flags/partition, GEMM tile/stage, window query count, grid dimensions, workgroup/subgroup requirements and per-frame timestamp spans. Family totals and dispatch-span totals are actual per-frame series. The separately labeled sum of independently minimized spans is only a localization statistic; minima from different frames must not be called a measured frame time.

Promotion checks use three or more interleaved pairs, five or more warmups and 30 or more measured frames per run. The whole-network median and P95 must not regress more than 2%, including each pair. A new default also needs at least 5% lower network median. Operator records need at least 5% lower median, one identified variant and matching uninstrumented network evidence. Profile runs require their matching ordinary benchmark manifest for promotion.

Preserving tuning additionally requires the strict operator suite, all 75 model boundaries plus head/capture-production at 320, and target head/capture-production artifacts. Actual selected and baseline policies must agree across those suites and their performance records; identical SPIR-V hashes alone cannot distinguish different specialization constants. Tools rehash/recompare the actual binary proof files and raw chronological measurements when generating tuning records. Editing a summary eligibility boolean cannot qualify a variant. The re-audited Q32 qualification and generated tuning report pass preservation/default eligibility; their scene-quality, motion-review and complete-game-benchmark gates remain false.

Changed K32/final arithmetic cannot enter preserving automatic tuning. Its separate evaluation requires matched **unclamped scene-linear pre-FSR RGB**, fixed data range 1, PSNR at least 40 dB and SSIM at least 0.99, prescribed scene coverage and both identical/evolved history modes. These are numerical thresholds; human motion review and complete game performance validation remain separate. HDR display evaluation is deferred for the initial SDR target, but scene-linear highlight errors in SDR content still matter. Existing synthetic HDR/reset and temporal threshold failures remain documented in [AMD.md](AMD.md).

## Selection behavior

| CLI option | Values and meaning |
| --- | --- |
| `--amd-kernels` | `auto`, `baseline`, `optimized` |
| `--amd-arithmetic` | `k16` default; `k32`/`final` explicit experiments |
| `--amd-gemm` | `shared`, `packed`, `direct`; direct requires stage K16 |
| `--amd-tile-n` / `--amd-stage-k` | `16`, `32`, `64`; defaults 16/16 |
| `--amd-window-queries` | `64` default compact path, `16` or `32` smaller workgroups |
| `--amd-fusion` | `0` default; `1` shorthand selecting both C32 routes |
| `--amd-ffn32-fusion` / `--amd-qkv32-fusion` | `0`/`1` independent C32 route overrides |
| `--amd-expert-fusion` / `--amd-block-fusion` | `0` default; `1` respective experimental paths |
| `--amd-hardware-publication` | `0` default; `1` rejected optional half-publication experiment |
| `--amd-tuning` | Path to a generated qualified session tuning file |

The environment equivalents use `DLSS5VK_AMD_` followed by `KERNELS`, `ARITHMETIC`, `GEMM`, `TILE_N`, `STAGE_K`, `WINDOW_QUERIES`, `FUSION`, `FFN32_FUSION`, `QKV32_FUSION`, `EXPERT_FUSION`, `BLOCK_FUSION`, `HARDWARE_PUBLICATION`, or `TUNING`. Explicit CLI values are applied before device creation. Baseline refuses non-default arithmetic/tile/query/fusion/publication overrides and fails explicitly when its 34,816-byte attention requirement exceeds the device limit.

`amdcheck` and the Python collection, analysis, qualification, tuning and merge tools accept `--comparison-anchor legacy|compact64|qualified32`; the PowerShell collector uses `-ComparisonAnchor`. The historical default is `legacy`. New direct-GEMM recipes use `qualified32`: optimized shared GEMM/K16/N16/stage16/Q32 with all experiments off. The earlier attention recipes below retain `compact64`, whose baseline is legal Q64 with the same shared/K16/N16/stage16 policy. Collection manifests, exact proofs, assessments and tuning records identify the anchor and bind its actual selected policy and shader identity. Unlabeled historical artifacts denote legacy; a compact64 label or flag never silently remaps those artifacts. This comparison option has no runtime environment equivalent.

For the pinned development identity, auto has a qualified Q64/N16/stage16/K16 compact fallback with fusion and optional hardware half publication off. A generated `amd-tuning.json` beside the shaders, or explicit tuning path, can select a fully qualified session policy only for matching padded geometry, device, driver, model and shader aggregates. Each graph/resize starts again from the immutable requested policy before resolving the pinned fallback and current tuning; invalid tuning cannot retain a previous geometry's Q32 selection. Explicit diagnostic overrides retain their requested settings and explicit tuning mismatches fail visibly. An unknown identity without a legal qualified fallback refuses initialization; the game host retains its ordinary upscaler continuation instead of running the known over-budget legacy pipeline.

The earlier legal-anchor Q32/shared tuning is [rx9070xt-26.9.1-k16.json](performance/rx9070xt-26.9.1-k16.json), SHA-256 `6f2298c945eaa54527d9642a31a9d47d29ebd1d768a128442f9f9f4374c6017d`. Each record binds its complete measured session policy, including GEMM staging, query count and all fusion/publication flags. Missing or inconsistent selected-policy evidence rejects that cache before variant modules are loaded, preserving the legal qualified fallback. The earlier game runs used the prior tuning artifact, recorded with those runs. Updating the packaged tuning does not rewrite their provenance.

Per-shape records carry operator evidence, but the current runtime applies the fully measured **session policy**. Merging independently winning per-shape configurations does not approve an unmeasured heterogeneous network; the merge tool explicitly leaves automatic default eligibility false. Evidence for the pinned fallback covers the stated 320 and target fixtures, not every game or resolution.

The new target direct tuning is [rx9070xt-26.9.1-direct-k16.json](performance/rx9070xt-26.9.1-direct-k16.json), SHA-256 `058c4dde3ba0e679ec3bf3bdd247295459b14f4bbc9908fb3624e7e9d046692e`, with 46 records. The complete ordinary network and per-operator gates, strict outputs, native lifecycle, bounded history replay and automatic cache GPU selection pass for this policy. See [the direct delivery recipe](amd-gemm-delivery.md#reproducible-isolated-builds). Historical tuning remains unchanged.

## Reproducing qualification

The following recipe preserves the earlier legal Q64-to-Q32 attention
comparison. New direct-GEMM work uses the `qualified32` anchor and the isolated
recipe linked above.

Run from a source checkout using PowerShell 7, the fetched MSVC/glslang toolchain and Python 3.10 or later. NumPy is needed for SSIM image comparisons. Stop other GPU workloads before timed runs. Use a new ignored experiment directory; snapshots, collectors and qualification outputs refuse overwriting evidence. `$model` must be the locally verified imported model directory; no NVIDIA DLL is executed or copied by these commands.

```powershell
$case = 'build/performance/reproduce-q32-20261002'
$model = 'models/imported/open-nr'
$frozen = "$case/frozen"
$queries = 32

# Build the core/runtime before freezing the selected SPIR-V files.
./scripts/build.ps1 -Backend amd
./scripts/build_game.ps1 -SkipCore
$cxx = '<absolute path to the MSVC cl.exe used for this build>'
./scripts/freeze_amd_baseline.ps1 -OutputDirectory $frozen -ModelDirectory $model `
  -CxxCompiler $cxx

$baselinePolicy = @('--amd-kernels','optimized','--amd-arithmetic','k16',
  '--amd-tile-n','16','--amd-stage-k','16','--amd-window-queries','64',
  '--amd-fusion','0','--amd-expert-fusion','0','--amd-block-fusion','0',
  '--amd-hardware-publication','0')
$candidatePolicy = @('--amd-kernels','optimized','--amd-arithmetic','k16',
  '--amd-tile-n','16','--amd-stage-k','16','--amd-window-queries',"$queries",
  '--amd-fusion','0','--amd-expert-fusion','0','--amd-block-fusion','0',
  '--amd-hardware-publication','0')

./build/dlss5vk.exe amdcheck --baseline-shaders "$frozen/shaders" `
  --shaders build/shaders --fixture "$case/operators" `
  --comparison-anchor compact64 --json "$case/operators.json" @candidatePolicy

./build/dlss5vk.exe modelcheck --backend amd --model $model `
  --shaders "$frozen/shaders" --width 320 --height 320 --frames 3 `
  --intermediates --fixture "$case/baseline320" @baselinePolicy
./build/dlss5vk.exe modelcheck --backend amd --model $model `
  --shaders build/shaders --width 320 --height 320 --frames 3 `
  --intermediates --fixture "$case/candidate320" `
  --reference "$case/baseline320" --require-exact @candidatePolicy

./build/dlss5vk.exe modelcheck --backend amd --model $model `
  --shaders "$frozen/shaders" --width 1707 --height 960 --frames 3 `
  --head-only --intermediates --fixture "$case/baseline-target" @baselinePolicy
./build/dlss5vk.exe modelcheck --backend amd --model $model `
  --shaders build/shaders --width 1707 --height 960 --frames 3 `
  --head-only --intermediates --fixture "$case/candidate-target" `
  --reference "$case/baseline-target" --require-exact @candidatePolicy
```

Modelcheck includes diagnostic downloads; its timings do not supply the ordinary benchmark protocol. The collector clears inherited `DLSS5VK_` environment variables and records isolated child overrides. Manual commands should also use a shell without unrelated tuning/scalar/debug overrides. The following collector uses the same current executable for both roles and explicitly selects optimized Q64 as its comparison anchor and optimized Q32 as its candidate. Their actual selected policies and shader aggregates must agree with the corresponding model fixtures. The older eight-shader legacy snapshot does not contain the optimized anchor; retain it as historical evidence and create the complete current snapshot above.

The snapshot records commit/dirty state, actual executable/runtime/SPIR-V hashes, optional shader-directory tuning JSON, model manifest and device identities, glslang identity, and C++ compiler file hash/version. If `-CxxCompiler` is omitted, it only detects `cl` already on PATH; an unavailable compiler is recorded as null. It also hashes relevant source files under `src`, `shaders`, `game`, `tests` and `scripts`, excluding model/capture/generated/build inputs. This is an immutable source-input inventory, not a source archive or proof that binary bytes were reproduced from those inputs. Preserve the corresponding source/diff separately, especially for a dirty checkout. The snapshot's device-info command initializes Vulkan, so it is separate from CPU-only checks.

```powershell
python tools/tune_amd.py collect --executable build/dlss5vk.exe --model $model `
  --shaders build/shaders --output "$case/bench" --mode bench `
  --width 1707 --height 960 --pairs 3 --warmup 5 --frames 30 `
  --kernels optimized --arithmetic k16 --tile-n 16 --stage-k 16 `
  --window-queries $queries --comparison-anchor compact64

python tools/tune_amd.py collect --executable build/dlss5vk.exe --model $model `
  --shaders build/shaders --output "$case/profile" --mode profile `
  --width 1707 --height 960 --pairs 3 --warmup 5 --frames 30 `
  --kernels optimized --arithmetic k16 --tile-n 16 --stage-k 16 `
  --window-queries $queries --comparison-anchor compact64
python tools/tune_amd.py analyze "$case/profile/interleaved.json" `
  --network-manifest "$case/bench/interleaved.json" `
  --comparison-anchor compact64 --output "$case/profile-network-assessment.json"

python tools/qualify_amd_model.py --baseline "$case/baseline320" `
  --candidate "$case/candidate320" `
  --baseline-benchmark "$case/bench/pair-01-baseline.json" `
  --candidate-benchmark "$case/bench/pair-01-candidate.json" `
  --comparison-anchor compact64 --output "$case/model320-exact.json"
python tools/qualify_amd_model.py --target-only --baseline "$case/baseline-target" `
  --candidate "$case/candidate-target" `
  --baseline-benchmark "$case/bench/pair-01-baseline.json" `
  --candidate-benchmark "$case/bench/pair-01-candidate.json" `
  --comparison-anchor compact64 --output "$case/target-exact.json"
python tools/tune_amd.py qualify --arithmetic k16 --exact "$case/operators.json" `
  --exact "$case/model320-exact.json" --exact "$case/target-exact.json" `
  --comparison-anchor compact64 --output "$case/qualification.json"
python tools/tune_amd.py tuning --performance "$case/profile-network-assessment.json" `
  --qualification "$case/qualification.json" --comparison-anchor compact64 `
  --output "$case/amd-tuning.json"
```

Check each command's exit status before continuing. A comparison failure writes failed evidence or rejects provenance; it does not become qualified because later commands were entered. Keep proof bundles, fixtures and raw interleaved records with the JSONs because qualification/tuning rechecks them. A generated tuning file is not a game-quality release approval. Point a local `--amd-kernels auto --amd-tuning "$case/amd-tuning.json"` run at the same model, geometry and shaders before considering packaging.

If two separate experiments have independently qualified evidence using the same explicit anchor, merge their per-shape records with the same label. The merged configuration remains ineligible for automatic default selection until it has its own whole-network evidence:

```powershell
python tools/tune_amd.py merge `
  --candidate "$case/profile-network-assessment.json" "$case/qualification.json" `
  --candidate '<other experiment>/profile-network-assessment.json' '<other experiment>/qualification.json' `
  --comparison-anchor compact64 --output "$case/merged-tuning.json"
```

### CPU-only checks

These do not load a model or execute a Vulkan/game workload:

```powershell
python -m unittest discover -s tests -p 'test_amd_*.py'
./tests/package_tests.ps1
./tests/install_tests.ps1
./tests/freeze_amd_baseline_tests.ps1

$vcvars = & ./scripts/find_vcvars.ps1
$cpu = Join-Path (Get-Location) 'build/performance/cpu-tests'
New-Item -ItemType Directory -Force -Path $cpu | Out-Null
foreach ($test in @('amd_config_tests','amd_selection_tests',
                    'shader_identity_tests','capture_request_tests',
                    'amd_fusion_validation_test','amd_window_resources_tests')) {
  $command = '"{0}" >nul && cl /nologo /std:c++20 /EHsc /O2 /DNOMINMAX /DVK_ENABLE_BETA_EXTENSIONS /D_CRT_SECURE_NO_WARNINGS /Isrc /Itools/Vulkan-Headers/include /Itools/volk /Fo"{1}/{2}.obj" /Fe:"{1}/{2}.exe" "tests/{2}.cpp" && "{1}/{2}.exe"' -f $vcvars,$cpu,$test
  cmd /c $command
  if ($LASTEXITCODE -ne 0) { throw "CPU check failed: $test" }
}

# Optional compatibility checks against existing locally built SPIR-V; still CPU-only.
& "$cpu/shader_identity_tests.exe" build/shaders
if ($LASTEXITCODE -ne 0) { throw 'Loaded shader aggregate compatibility failed' }
```

The selector regression passes 75 CPU checks, covering successful Q32 tuning followed by resize, replaced/invalid tuning, changed model/driver/shader identity, complete selected-policy binding and explicit diagnostic overrides. The window-resource regression passes 44 CPU checks of the unchanged declarations, exact device limits, invalid query sizes and explicit legacy rejection; it exercises [amd_window_resources.h](../src/amd_window_resources.h) without a Vulkan device. The shader-identity regression passes 24 CPU checks including the optional locally built SPIR-V compatibility checks: identities hash the exact word bytes supplied to module creation and remain bound to those loaded bytes when a source file changes afterward. Passing `build/shaders` checks baseline/Q64/Q32 aggregate compatibility. The snapshot-source check verifies that later source edits change a newly collected identity while leaving the frozen inventory unchanged; it does not invoke the snapshot's device query. An optional `./tests/source_package_build.ps1 -PackageDirectory '<new final package>'` checks a delivered corresponding-source build without GPU execution and retains its isolated workspace.

## Resource analysis workflow

The pinned portable RGA helper analyzes SPIR-V without GPU execution or a driver/layer installation. It specializes a private copy, invokes bundled LLPC with explicit wave32, validates ELF metadata against RGA binary statistics, and retains hashes, source-closure metadata, ISA, LDS/register/scratch values and static WMMA counts. Wider staged loads are motivated by [AMD's RDNA4 WMMA guide](https://gpuopen.com/learn/wmma-guide-amd-rdna-4-gpus-part-2/); preserving this model still requires its K16 publications and actual regression tests.

```powershell
./scripts/analyze_amd_shaders.ps1 -FetchTool
./scripts/analyze_amd_shaders.ps1 -Spirv build/shaders/amd_window_optimized.spv `
  -Source shaders/amd_window_optimized.comp -Specialization @{10=16} `
  -DeclaredLdsBytes 22528 -OutputDirectory "$case/rga-window64"
./scripts/analyze_amd_shaders.ps1 -Spirv build/shaders/amd_window_small.spv `
  -Source shaders/amd_window_small.comp -Specialization @{10=16;14=32} `
  -DeclaredLdsBytes 15360 -OutputDirectory "$case/rga-window32"
./scripts/analyze_amd_shaders.ps1 -Spirv build/shaders/amd_gemm_optimized.spv `
  -Source shaders/amd_gemm_optimized.comp `
  -Specialization @{0=128;2=16;3=0;10=16;11=16;12=16;13=$false} `
  -DeclaredLdsBytes 5376 -OutputDirectory "$case/rga-gemm128-n16-stage16"

# This separate command DOES execute a model recording on the GPU.
./build/dlss5vk.exe shaderinfo --backend amd --model $model --width 1707 --height 960 `
  --filter window_attend @candidatePolicy > "$case/installed-driver-window.log"
```

Offline reflected descriptor layouts and compiler builds differ from the installed Windows pipeline. [RGA's manual](https://gpuopen.com/manuals/rga_manual/help_manual/) describes Vulkan compute-state `.cpso` files and warns that fallback offline resource/ISA output is less representative. Retain actual specialization values, descriptor/pipeline state and required subgroup state when moving from the portable helper to driver-based analysis. Source correspondence hashes alone do not prove how an input SPIR-V was built.

[The final Q32 offline scalar report](performance/rga-final-q32-rx9070xt-20261002.json) maps 33 exact compiled specializations to all 358 FP8 GEMM and 62 window-attention dispatches in each measured target frame. The other 93 operators are explicitly outside that family analysis. All inspected variants use wave32 and 128 invocations with zero reported scratch/spills and compiled LDS within 32 KiB. GEMM compiles to 5,632 bytes LDS (5,376 source bytes plus 256 compiler-added bytes), 22–32 VGPRs and 27–40 SGPRs; Q32 uses 15,360 bytes, 34 VGPRs and 29 SGPRs in the offline tool. Window heads 1/2/4/8/16, shifts 0/4 and shapes are push constants mapped to the shared Q32 specialization. Native static FP8 WMMA matches range with flags in GEMM and total six in Q32; these are not dynamic instruction counts. Input, specialized SPIR-V, ELF, ISA, statistics and tool hashes are revalidated, including reused reports. No raw profiler binaries or generated code artifacts are distributed.

For deeper analysis, [RGP supports RX 9000, Vulkan and Direct3D 12 on Windows](https://gpuopen.com/rgp/). Use a separate diagnostic run through Radeon Developer Panel/Service, save the profile under the experiment directory, and inspect WMMA delivery, occupancy, spills, LDS traffic, barriers and queue/fence waits. RGP's [official manual](https://gpuopen.com/manuals/rgp_manual/) covers the capture tools and those views.

On October 2, an RGP diagnostic file was successfully collected under ignored `build/performance/rgp-q32-20261002/q32-window.rgp` (476,120,628 bytes). RGP reports **truncated SQTT**: the inspected portion contains approximately the first 101 events of the command buffer, rather than the complete 513-dispatch graph. Its 17 reported pipelines and 31,793.473 microsecond profile duration describe that limited trace, not complete inference time. [The scalar evidence record](performance/rgp-rx9070xt-20261002.json) preserves actual local file/tool hashes and the observed UI values without distributing the raw capture.

Selected event 15 is the first C32 Q32 attention dispatch: grid 1x51,840x1, workgroup 128x1x1, wave32, 15,360 bytes LDS, 31 used/48 allocated VGPRs, 30 used/128 allocated SGPRs and scratch spill disabled. Its native ISA contains six static `v_wmma_f32_16x16x16_fp8_fp8` instructions, with the first at displayed ISA line 348. RGP's theoretical resource view reports 8 of 16 waves per SIMD, limited by LDS, and a 2,560-byte LDS-reduction suggestion. These confirm the inspected installed-driver pipeline's wave size, resources and static matrix instructions. They do not establish dynamic utilization: instruction timing says **"No wavefronts analyzed"**, and truncated SQTT prevents a complete-graph occupancy/timing conclusion. The six static instruction matches are not an executed-instruction count.

The local RDP CLI run succeeded without a separately prestarted RDS; that prestarted configuration had failed with `FailedToCreateRouter`. The capture tool applied its capture clock mode and restored clocks afterward. This externally instrumented diagnostic supplies no ordinary benchmark or game FPS samples, even though the application's own per-dispatch instrumentation flag was false.

## Bounded game capture and temporal replay

Run capture separately from performance. In the installed assets directory, an empty `capture.flag` requests one frame; a text integer from 1 through 120 requests a bounded sequence. Create the flag after the intended scene/history is ready. Removing it across an enqueue cancels/re-arms the request. Files are written under `open-nr/captures/` with sequence/ordinal metadata, process identity, model/shader hashes, controls and six required binary-file hashes.

```powershell
# Replace this with the exact installed assets directory; not a performance run.
$assets = '<game bin/x64/open-nr>'
Set-Content -LiteralPath (Join-Path $assets 'capture.flag') -Value '30' -Encoding ascii
# Wait for all requested captures, then remove the exact flag.
Remove-Item -LiteralPath (Join-Path $assets 'capture.flag')

# Use a copied capture-sequence directory outside game files.
python tools/tune_amd.py replay --capture-sequence '<local copied sequence directory>' `
  --executable build/dlss5vk.exe --model $model --shaders build/shaders `
  --game-shaders build/game/shaders --output "$case/game-replay" `
  --reference-backend reference --history-mode both --kernels optimized `
  --arithmetic k16 --tile-n 16 --stage-k 16 --window-queries $queries `
  --coverage sdr --coverage motion
```

Only label coverage actually demonstrated by the captured scene; repeated frames do not supply missing faces, cuts or exposure transitions. The strict sequence path requires a complete bounded request, explicit genuine-game provenance and required hashes. Do not add `--allow-nongame` to claim game acceptance. The previously misclassified live-world capture remains unchanged and cannot close this gate.

Identical-history replay uses the same recorded prior history for reference/candidate. Evolved replay starts each route from reset and carries that route's own previous composed history forward; an interrupted sequence requires a recorded reset. Source color/motion, seed, model, controls, dimensions, pre-exposure, jitter and frame ancestry remain matched. RGB comparisons are before FSR and display transfer. Runtime captures contain Vulkan F32 output before the final D3D12 RGBA16F resource conversion; predicted half-publication comparisons or separately captured D3D textures must be labeled accordingly.

Eight genuine Cyberpunk frames (1155-1162) at 1707x960 were subsequently replayed against the portable exact reference. Every matched scene-linear RGB frame passes PSNR >=40 dB and SSIM >=0.99 with fixed data range 1.0: minimum identical-history results are **49.536 dB / 0.998920**, and minimum independently evolved-history results are **48.519 dB / 0.998642**. The selected Q32/K16 replay matches captured preprocessing, F32 head and composed production buffers byte for byte on all eight frames. [Scalar replay evidence](performance/cyberpunk-replay-rx9070xt-20261002.json) excludes source buffers and imagery. The sequence shows camera movement and steam in an alley. It does not demonstrate faces, moving objects, cuts, exposure transitions or broader disocclusion coverage. Four spatial contact-sheet pairs received limited inspection; temporal flicker/ghosting acceptance remains pending. These passes do not override the synthetic HDR-highlight failures.

## Final alpha 2 game status

The published alpha 2 binaries (source `7f1cd3108133d8aee9bae505cb31542e236d13e0`, runtime `638de5aee97b65d5e091c5eb6af63df96cf38d729985cadff2b8cb79fe5c3e6c`) now have three warmed NR-off and three warmed NR-on complete Cyberpunk benchmark passes. Equal-pass averages are **97.66045 FPS off / 7.54553 FPS on**; the 2,916 complete exported NR-on frame times have median **132.55 ms**, P95 **133.89 ms** and P99 **134.637 ms**. These complete game-exported statistics are separate from PresentMon subsets and runtime-stage spans. [The final alpha 2 validation record](cyberpunk-alpha2-validation.md) and [its scalar evidence](performance/cyberpunk-alpha2-rx9070xt-20261002.json) retain the actual identities, repeat variation, settings and exclusions.

Closed final-alpha2 evidence qualifies five bounded world PresentMon subsets with the matching TimeInQPC present clock and three asynchronous completed-job brackets; off-pass2 is excluded because its end marker followed the results screen. Network medians are **123.699 / 123.694 / 123.655 ms**, and NR-plus-bridge medians are **124.953 / 124.964 / 124.952 ms**. Maximum observed sampled DXGI process-local usage is **9,607.879 MiB**; refresh occurs every 60 completed jobs, so this is not a true peak. Runtime jobs remain separate from game presents and complete engine-exported frames.

Eight final-alpha2 genuine fixed-camera SDR city frames (7085–7092) pass every composed-image threshold against the unchanged portable exact reference: minimum identical/evolved values are **51.32959 dB / 0.999792** and **50.52491 dB / 0.999676**. All 32 reference/candidate runs succeed; the 16 AMD candidates use Q32/K16, while reference runs retain the exact software schedule. Forty independently streamed buffer checks reproduce captured production exactly. This is not exact-reference byte parity: maximum RGB errors are **1.87991 / 1.90947**, requiring localized highlight and visual review. Exposure is unavailable with fallback1 throughout; no exposure-change or broad motion/face/cut/disocclusion coverage is established.

Ten minutes of active gameplay were not run and still require manual input; automated control did not establish sustained movement. Broad image/temporal review remains incomplete. Diagnostic capture readback is excluded from ordinary timing. Final cleanup is complete: 35 managed files were removed, processes/ledger/owned flags are absent, original settings were restored exactly, and models/captures were retained. The 8 ms NR-plus-bridge and 60 FPS targets and existing synthetic HDR-highlight quality gate remain unmet; NR stays disabled by default.

The later timing-helper correction pairs hybrid-trace `MsBetweenPresents` with
`TimeInQPC`, instead of the distinct `CPUStartQPC` clock. Eleven CPU tests pass;
newly named reports replace world-bound qualification while preserving raw and
original reports. Missing Dropped/FrameType fields remain unknown observations,
not zero drops/generation. This source-only analysis fix does not alter the
published binaries or any complete engine benchmark result.

## Source-only JSON hardening after alpha 2

A later CPU-only parser fix rejects malformed literals, duplicate decoded keys,
invalid strings/escapes/UTF-8 and invalid or unrepresentable numbers, with bounded
nesting. The reproducible regression script runs **167 checks** by default and
**168** with the local model manifest. The earlier parser-stage tuning selector passed **75**
valid-cache checks; the current selector passes **125**. A malformed `trux` cache fails visibly. Baseline/new parsed
output matches exactly for **36 actual JSON files / 208,437 typed nodes**.

```powershell
./tests/test_json_parser.ps1
```

The script builds only its CPU test executable in a unique ignored directory and
generates its malformed-cache witness there. The published alpha 2 assets and
their historical game/replay binaries retain their recorded identities and
original parser. New alpha 3 core/runtime binaries, native GPU/bridge validation
and the fresh source rebuild include the fix and pass their recorded checks.

## Historical game evaluation before the final alpha 2 tests

The measurements and checkpoint statuses below belong to earlier application binaries. They remain unchanged evidence and must not be substituted into the final alpha 2 benchmark record above.

The **older implementation** completed three Cyberpunk benchmark passes at 4.56, 4.56 and 4.57 average application FPS, plus a 600-second limited live-world session. In-game frame generation was off; driver AFMF was not verified, so those records do not assert real-rendered FPS. They used a different runtime/shader configuration and must not be reused as optimized-kernel results. See [the validation record](rx9070xt-validation.md) for exact settings, hashes, selected PresentMon intervals and scope.

An initial optimized-package launch crashed with NR disabled in the unchanged OptiScaler host's cached NVIDIA Streamline capability-spoofing path, before the neural runtime loaded. The AMD/FSR package default now sets `[Spoofing] StreamlineSpoofing=false`; subsequent runs reached gameplay and completed the measurements below.

At 2560x1440 output, FSR Quality, ray tracing off and target High graphics fields (the game labels the edited preset Custom), three earlier NR-off built-in benchmarks reported **99.58 / 99.32 / 101.74 average FPS**. The first ordinary NR-on built-in benchmark reported **7.51 average FPS**, 7.44 minimum and 7.59 maximum, with 972 frames over 129.51 seconds. In-game frame generation and the Cyberpunk driver profile's AFMF were observed off; CSV rows alone do not prove those settings. At that checkpoint, two additional NR-on passes and the ten-minute active gameplay test had not been completed.

These game runs and the complete genuine eight-frame replay used CLI `1c5f969fb3474d53956b959cfafea423ebebb64b26c6a7b72374f889fc96efa4` and runtime `0c5c05c57ff7faa6c96c4d84dd84c15ba6d299e969985fbddf5093f68cdd91cf`, with the prior Q32 tuning. The earlier legal-attention network benchmark uses its separately identified CLI. Final package identity updates do not turn these earlier game runs into fresh release-binary benchmarks, although the selected shader aggregates are unchanged.

PresentMon analysis uses explicit process/swap-chain selection and independently observed QPC boundaries. These conservative world subsets are separate from the full built-in benchmark results:

| Bounded sample | Intervals | Window | Mean | Median | P95 | P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| NR-off pass 2 | 3,124 | 28.98585 s | 9.277723 ms | 9.29935 ms | 10.67739 ms | 11.726457 ms |
| First ordinary NR-on pass | 737 | 98.426 s | 133.2312 ms | 133.2766 ms | 135.66924 ms | 136.864388 ms |

The NR-on end boundary is conservatively qualified one second before the last confirmed world observation; the original end included a menu transition. No latency-based filtering is applied. NR-off pass 2's subset is not its full 99.32 FPS result, and the NR-on subset is not the full 129.51-second result. Their percentiles must not be relabeled as full-pass percentiles. Local result and boundary records are under ignored `build/performance/game-20261002/`, including `off-2-presentmon.json` and `on-1-presentmon-qualified.json`. Runtime completion rows are not joined to individual game presents.

[Published scalar game evidence](performance/cyberpunk-rx9070xt-20261002.json) also records an asynchronous completed-job bracket after the observed world-start anchor through the delayed original end anchor: 840 completed NR jobs, median inference **124.429 ms**, median GPU NR-plus-bridge span **125.711 ms** and P95 **126.274 ms**, with sampled DXGI process-local VRAM peak **9,645.352 MiB**. This bracket can include a menu transition at its end and is not the conservative QPC-aligned world interval above; completion rows are never joined to presents. The ending session counters show 3,070 submitted and zero bypassed. VRAM refreshes every 60 jobs, and the neural allocation counter excludes weights, game resources and shared buffers. The existing user driver FSR upscaling override remained enabled in both conditions, so the selected game setting does not independently establish the exact effective upscaler version.

Further UI-driven runs in that earlier evaluation were blocked by a Windows Security prompt from the profiling service. The user subsequently dismissed it before the final alpha 2 tests. The earlier checkpoint had two additional NR-on runs, active gameplay and broad motion review incomplete; that limitation did not change its measured first-pass result or close any release gate. Its temporary test package was removed and original user settings restored byte for byte. Imported models and local captures were preserved; the earlier restored settings SHA-256 is `d1b2419f1da1a308f13b594b222d77144f5b42f0836b2ae92beefb0da18607c6`. Cleanup for the separate final alpha 2 test session is also complete and is independently recorded in its validation record.

Status at the end of that earlier-binary evaluation:

| Earlier optimized game evidence | Historical checkpoint status |
| --- | --- |
| Exact packaged runtime/host/SPIR-V/tuning hashes | Snapshot was pending at this checkpoint; the separate final identity record is now available |
| Final offline/RGP pipeline delivery analysis | Partial RGP Q32 wave32/resources/native FP8 WMMA confirmed; truncated SQTT and no analyzed wavefronts leave dynamic/full-graph analysis unqualified; final Q32 offline specializations recorded |
| NR-off startup with the safe packaged INI | Pass through three built-in benchmark runs |
| NR-off baseline, matching 1440p/FSR settings | 99.58 / 99.32 / 101.74 average FPS; bounded pass-2 PresentMon subset reported separately |
| NR-on warmup plus three measured passes | Earlier first ordinary pass 7.51 FPS; two additional passes were pending at this checkpoint |
| Full frame median/P95/P99 and stage timing/VRAM | Bounded PresentMon subsets above; completed-job bracket has 125.711 ms median NR-plus-bridge and 9,645.352 MiB sampled peak; full-pass distributions pending |
| Bypass counts, resize/cancel/drain and host continuation regressions | Eight-frame native harness passes; target-to-192 resize selects Q32 then Q64; observed NR-on completion bracket ends at 3,070 submitted / 0 bypassed |
| Ten-minute updated live-world session | Pending at this checkpoint; final active gameplay still needs manual input |
| Genuine bounded scene captures, identical/evolved replay | Eight genuine 1707x960 frames pass every numerical threshold in both modes; captured production head/composition reproduce byte for byte |
| Faces, motion, exposure, cuts, disocclusion and human artifact review | Pending |

The next kernel work should use the new direct profile: FP8 GEMM has a 48.796 ms family median and window attention 21.033 ms, with 80.805 ms whole-frame median. These are independently summarized instrumented spans, not ordinary timings or additive family medians. Inspect installed-driver operand supply, publication and occupancy, probe a correctly rounded conversion against the existing witnesses, and measure independent fusion routes before redesigning them. The exact F24 adapter/head and software reference remain useful anchors. A different model, skipped NR updates, frame generation, old-result reuse or a translated game backend would require their own behavior and quality decision; they are not the speedup reported here.

[The remaining-work record](amd-performance-next-steps.md) identifies source gaps separately from acceptance evidence. Independent AMD FFN/QKV controls are implemented and each passes the 320 model comparison with direct GEMM; separate target performance and lifecycle/replay promotion remain work. Pooling/upsampling variants and C512 split-FFN fusion are partial. The updated GEMM and attention profile remains the optimization priority. Three warmed game benchmarks per condition are complete; broad quality/temporal review, highlight acceptance and active gameplay remain open; slower fusion remains disabled by default.

The implemented experiments provide a measurable reduction and stricter reproducibility. They have not reached the requested performance budget or completed the game release gates.
