# RX 9070 XT neural rendering performance research

Research checked October 1, 2026, for this repository's 71-block neural renderer on Windows and the Radeon RX 9070 XT. The strongest next step is an AMD-specific fused Vulkan implementation, informed by a public implementation of the same model and AMD's RDNA4 WMMA guides. HIP and DirectX matrix APIs are useful comparison experiments. All optimizations below are proposals; this research does not establish a new local speedup.

The survey covers official architecture guidance, neural rendering papers, open kernels, same-model implementations, and platform compatibility. It prioritizes sources with code or measurements relevant to RDNA4. It is a broad survey of useful public work, not a claim that every publication has been found.

## Local measurements and constraints

The device was rechecked with `build/dlss5vk.exe info --backend amd`: RX 9070 XT, Adrenalin 26.9.1, LLPC, Vulkan 1.4.349, and E4M3 16x16x16 matrices with FP32 accumulation. The reported default subgroup is 64; FP8 GEMM and matrix-attention pipelines request wave32. See [arithmetic documentation](amd-numerics.md) and the [validation record](rx9070xt-validation.md).

Existing idle measurements at valid 1707x960, padded to 1728x960:

| Measurement | GPU time | Meaning |
| --- | ---: | --- |
| Warmed inference median | 217.949 ms | Three frames, 513 dispatches; excludes bridge, FSR and presentation |
| Dispatch profile sum | 210.206 ms | Sum of minimum spans for individual dispatches, not a measured complete frame |
| Listed window attention | 108.164 ms | At least 51.5% of profile sum |
| Listed FP8 GEMMs | 78.387 ms | At least 37.3% of profile sum |
| Listed window normalization | 3.266 ms | Two entries visible in the truncated kernel table |
| Global attention | 2.115 ms | Approximately 1.0% of profile sum |
| Exact half adapter and head | 3.176 ms | Combined listed time |
| 400 trivial dispatches and barriers | 0.385 ms | Small-buffer overhead probe; does not measure cache flushes with large tensors |

Sources are the existing local `build/interop/global-matrix-bench1707.log` and `global-matrix-profile1707.log`. The profile prints only the largest 40 aggregated kernel entries; family sums above are lower bounds. Listed entries cover 195.108 ms, leaving 15.098 ms unlisted. Two full-resolution attention passes cost 17.479 and 17.234 ms. Blocks 0 and 70 together account for 50.003 ms of stage spans.

Reaching an 8 ms inference budget from the recorded median requires approximately 27.2 times less GPU time, before game overhead. Halving the listed attention spans would save approximately 54.1 ms on that profile accounting, which is substantial but insufficient by itself. The tiny-dispatch probe and small global-attention share argue for improving arithmetic delivery and intermediate traffic first.

The graph already reuses scratch, reducing requested activation memory from approximately 3513 MiB to 2223 MiB at the target geometry. Reuse does not remove tensor reads and writes. The full-resolution QKV, FFN and normalized QKV buffers still request 303.75, 202.5 and 151.875 MiB respectively. These are allocation requests, not total measured VRAM consumption.

## A faster public implementation of the same model

[DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD/tree/82560c4fbfaac347fc5e22c22025191402ae916b) is the highest-priority external reference. Its pinned README reports v0.0.3 network-only RX 9070 XT measurements:

| Model resolution | Windows | Linux |
| --- | ---: | ---: |
| 1920x1080 | 7.21 ms | 5.60 ms |
| 2560x1440 | 12.59 ms | 9.70 ms |
| 3840x2160 | 27.32 ms | 21.89 ms |

These are author-reported measurements, not reproduced on this machine. Inputs, numerical schedules, compiler paths and timing methodology differ from our baseline. The Windows package is experimental and translates games to Vulkan; its integration differs from our native D3D12 bridge. Nevertheless, a faster implementation on the same GPU is strong reason to investigate software inefficiency. [Pinned README](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/README.md).

Its code provides concrete implementation references:

- [Fused Swin body](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/windows/shaders/rdna4/include/fswin_body.glsl) and [pipeline variants](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/windows/shaders/rdna4/pipelines.json): specialize channel widths and fuse operations, including input, pooling, upsampling and output variants.
- [Matrix helpers](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/windows/shaders/rdna4/include/coopmm.glsl): packed operand loads, register epilogues, explicit unrolling, and a measured fragment layout for subgroup reductions. The layout assumption requires a probe on our Windows driver before use.
- [Transposed expert FFN](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/windows/shaders/rdna4/ffwd3_t.comp): chains GEMMs and FP8 publications in registers; includes alternatives for weight reuse across token tiles.
- [Fused QKV attention](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/windows/shaders/rdna4/attn.comp): explores staging, transposed attention, occupancy, and packed conversions.

Numerical policy is a material difference: both implementations use FP32 hardware accumulation, but the external Swin presets set `NR_ACC_F16=0`, omitting intermediate half publication. Its attention documents optional half publication after two K16 steps. Our accelerated baseline publishes after every K16 and retains a prescribed half reduction tree. A wholesale transplant would require fresh quality validation. Its matrix helper also documents a failed optimization where narrowing then widening a fragment lost intended rounding during compilation. A syntactically present conversion is insufficient proof that a publication survived.

The project publishes [NGX comparisons](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/docs/ngx-verification/NGX-VERIFICATION.md) using its Linux v0.0.2.5 build against an RTX 5090 running NVIDIA's DLL. Reported 1080p reset output scores 45.56 dB PSNR and 0.9961 SSIM; moving tests use four constructed ten-frame sequences. These are 8-bit RGB quality results from a different build and platform than the Windows v0.0.3 timings above. They do not validate our scene-linear HDR composition or Cyberpunk temporal behavior.

The repository code is [MIT licensed](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/LICENSE). Inspect source and retain attribution for any adopted code. Only source files and documentation were inspected during this research; no external runtime, installer or model was executed.

## AMD architecture guidance

AMD published three practical RDNA4 guides on June 2, 2026:

| Primary source | Technique | Proposed application |
| --- | --- | --- |
| [WMMA guide part 1](https://gpuopen.com/learn/wmma-guide-amd-rdna-4-gpus-part-1/) | Keep intermediate GEMM outputs in registers and arrange the next product around fragment layout | Fused FFN and small-channel blocks |
| [WMMA guide part 2](https://gpuopen.com/learn/wmma-guide-amd-rdna-4-gpus-part-2/) | Supply pairs of FP8 or INT8 K16 operations using wider loads; the example uses INT8 | Stage K32 or K64 while retaining separate K16 arithmetic and rounding |
| [WMMA guide part 3](https://gpuopen.com/learn/wmma-guide-amd-rdna-4-gpus-part-3/) | Demonstrates an FP16 identity WMMA product to transpose register data | Compare against shuffle or LDS transpose in a controlled prototype; FP8 equivalence is unqualified |

The [July 2025 matrix-core introduction](https://gpuopen.com/learn/using_matrix_core_amd_rdna4/) explains gfx12 WMMA builtins and how its fragment layout differs from RDNA3. These are HIP examples; use their hardware principles when designing Vulkan kernels.

[KHR cooperative-matrix GLSL](https://github.com/KhronosGroup/GLSL/blob/main/extensions/khr/GLSL_KHR_cooperative_matrix.txt) leaves component mapping implementation-dependent and restricts constructors to the same matrix use. An accumulator cannot generally become an A or B fragment through a constructor. Start portable fusion with an LDS bridge, then qualify explicitly gated driver-specific register paths. Do not assume the NVIDIA cooperative-matrix2 operations used by upstream shaders exist on Radeon.

[Radeon GPU Analyzer](https://gpuopen.com/rga/) can inspect ISA, register pressure and LDS/scratch usage. [Radeon GPU Profiler](https://gpuopen.com/rgp/) supports Windows RX 9000 Vulkan profiling and exposes instruction timing, wave occupancy and barriers. Use actual compiled resource reports and captures to distinguish insufficient occupancy, bank conflicts, spills, scalar conversion overhead and poor WMMA delivery. The [RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/) provides supporting API guidance.

## Open kernels and practical platform support

These sources are kernel references or bounded backend experiments, not dependencies required by the current Vulkan runtime.

| Source | Useful evidence | Applicability |
| --- | --- | --- |
| [AITER](https://github.com/ROCm/aiter) | README now lists gfx1201 R9700 as experimental, with many Triton, FlyDSL and HIP operators | Same architecture, but individual kernels and Windows need qualification; many CK and assembly variants remain CDNA-specific |
| [AITER gfx1201 attention](https://github.com/ROCm/aiter/blob/main/aiter/ops/flydsl/kernels/flash_attn_func_gfx1201.py) | Real wave32 WMMA attention with pipelined loads and padded LDS | FP16/BF16; head dimensions must be at least 64 and divisible by 32. Our 32-channel window heads and fixed arithmetic need a new specialization |
| [AITER issue 5229](https://github.com/ROCm/aiter/issues/5229) | September 2026 report of one gfx1201 attention tile exceeding LDS and a build missing device objects | Configuration-specific evidence to check tile sizes and compiled targets |
| [CK release notes](https://rocm.docs.amd.com/en/docs-7.2.0/about/release-notes.html) | CK 1.2 adds gfx12 WMMA FMHA and wave32 | Current CK is broader than older Instinct-only descriptions; particular operations still need device checks |
| [rocWMMA precision support](https://rocm.docs.amd.com/projects/rocWMMA/en/latest/api-reference/data-type-support.html) | gfx1200/gfx1201 FP8 and FP16 matrix modes | Good controlled HIP reference. Check OCP E4M3 rather than assuming CDNA FNUZ/NANOO encoding |
| [llama.cpp RDNA cooperative-matrix commit](https://github.com/ggml-org/llama.cpp/commit/70c4e15) | INT8 KHR matrix shader for RDNA3/4 | Borrow packed staging and tiling; integer requantization of this model is separate accuracy work |
| [VulkanForge](https://github.com/maeddesg/vulkanforge) | Native FP8 Vulkan and cooperative-matrix attention on RX 9070 XT | Linux RADV/Mesa path and LLM benchmarks; useful source reference, GPL-3.0 code |
| [tiny-rdna4-nn](https://github.com/Painter3000/tiny-rdna4-nn) | gfx1201 R9700 port with hipBLASLt and a width-64 rocWMMA fused MLP | Qualified on Ubuntu/ROCm; useful fusion example, not general FullyFusedMLP or Windows validation |
| [tiny-rocm-nn](https://github.com/ZJLi2013/tiny-rocm-nn) | Another HIP/rocWMMA MLP port | Additional design reference; limited direct evidence for consumer Windows |
| [FlashAttention implementation](https://github.com/Dao-AILab/flash-attention) | AMD CK backend now lists RDNA3/4; AMD Triton path exposes tuning | Linux documented; Windows compilation insufficiently tested. FA3/FA4 NVIDIA-specific paths are distinct |
| [RDNA3 FlashAttention POC](https://github.com/Repeerc/flash-attention-v2-RDNA3-minimal) | Actual rocWMMA attention and numerical tests with Windows RX 7900 XTX benchmarks | HIP/ZLUDA reference; RDNA4 and Vulkan need qualification, attention-bias support is unfinished |

The [current ROCm compatibility matrix](https://rocm.docs.amd.com/en/latest/compatibility/compatibility-matrix.html) includes RDNA4/gfx1201 and Windows 11 25H2; the [ROCm installer](https://rocm.docs.amd.com/en/latest/install/rocm.html) publishes a Windows gfx120X ROCm 10.0 package. Native Windows HIP is therefore a plausible experiment. This does not make every ROCm library, AITER operator or graphics interop path supported.

[Upstream Triton](https://github.com/triton-lang/triton) documents Linux support, while [Triton 3.8](https://github.com/triton-lang/triton/releases/tag/v3.8.0) includes RDNA4 compiler work. Use it as a Linux/WSL research path until an exact native Windows toolchain is qualified. A HIP or Triton rewrite must demonstrate a kernel advantage large enough to justify its new deployment and resource-sharing costs.

## Attention algorithms and numerical limits

[FlashAttention](https://arxiv.org/abs/2205.14135) reduces traffic by tiling attention and fusing its stages. The [online softmax normalizer](https://arxiv.org/abs/1805.02867) maintains normalization state without materializing all scores. These are useful dataflow ideas; their published hardware speedups cannot be transferred to this renderer.

Our windows have 64 keys and 32 channels per head. Keeping a whole window in registers or compact LDS may beat a general large-sequence kernel. More importantly, `nrWindowExp` is a prescribed half/bit approximation, and `softmaxSum` uses a fixed rounded tree. Standard exponential, maximum subtraction, online renormalization or a different sum order can change the network's published bytes. Learn from FlashAttention's traffic reduction while preserving the current schedule in the first variants; label changed arithmetic explicitly in later variants.

## Windows neural shading experiments

| Primary source | What is available | Use here |
| --- | --- | --- |
| [AMD MiniDXNN](https://github.com/GPUOpen-LibrariesAndSDKs/MiniDXNN) | MIT HLSL MLP inference/training and neural texture compression using SM6.10 LinAlg | Real RDNA4 Windows research option; tested FP16, preview tooling and Developer Mode. Prototype one operator before considering a D3D12 backend |
| [D3D12 LinAlg preview](https://devblogs.microsoft.com/directx/d3d12-linalg-preview/) | Thread, wave and threadgroup matrix/vector interfaces; AMD RX 9000 preview support | Query supported data combinations and preview driver requirements; not a drop-in Vulkan facility |
| [DirectX ML direction](https://devblogs.microsoft.com/directx/evolving-directx-for-the-ml-era-on-windows/) | Separates inline shading, batched inference and graph optimization | Supports investigating whole-graph fusion; its private-preview graph compiler is not an available default backend |
| [Slang CoopMat](https://docs.shader-slang.org/en/latest/external/core-module-reference/types/coopmat-04/) | KHR matrix abstraction with device-specific shapes | Possible shader authoring route; advanced operations can require NVIDIA extensions |
| [Slang neural interface via CoopMat](https://github.com/shader-slang/slang/pull/9512) | Merged January 2026 interface work | Inspect reusable patterns; compiler support does not certify our driver's behavior |
| [Slang neural shading course](https://github.com/shader-slang/neural-shading-s26) | Neural shaders and differentiation examples | The current Vulkan neural examples explicitly require NVIDIA; educational source |

MiniDXNN currently specifies Windows 11 Developer Mode, Agility SDK 1.721-preview and DXC 1.10.2605.4. Record exact tools and driver requirements when reproducing it; these moving preview versions are not an instruction to change the game installation.

## AMD neural rendering research

These works help with future model design and temporal quality. They solve different tasks from the fixed generative appearance network in this repository.

| Research or SDK | Contribution | Relevance to our objective |
| --- | --- | --- |
| [Neural Supersampling and Denoising](https://gpuopen.com/learn/neural_supersampling_and_denoising_for_real-time_path_tracing/) and [I3D 2025 paper](https://dl.acm.org/doi/full/10.1145/3728297) | Joint denoising/upscaling with shared and lightweight feature branches | Ideas for a cheaper replacement model; requires training and different inputs |
| [FSR Redstone](https://gpuopen.com/learn/amd-fsr-redstone-developers-neural-rendering/) and [SDK 2.3](https://gpuopen.com/learn/amd-fsr-sdk-2-3-blog/) | AMD neural upscaling, frame generation and ray regeneration | Integration and inference precedent; these operations are not the same as same-resolution generative NR |
| [Radiance Cache manual](https://gpuopen.com/manuals/fsr_sdk/techniques/radiance-cache/) | Online neural prediction of secondary-bounce illumination | Small-network training/fusion ideas; needs renderer/path-tracing integration |
| [Lightweight attention indirect illumination](https://gpuopen.com/learn/lightweight-attention-based-indirect-illumination/) | SIGGRAPH 2026 attention from camera and reflective shadow map features | Future model research; cited 45.56 ms at 512 squared is unoptimized FP32 on MI250, not a Radeon performance result |
| [Temporally stable generative illumination](https://gpuopen.com/learn/temporally-stable-generative-illumination/) | ECCV 2026 one-step diffusion with geometric/material conditioning and temporal history | Interesting temporal and model-design work; no local RX 9070 XT latency established |
| [Generative caustics research](https://gpuopen.com/learn/genai-model-for-global-illumination/) | Eurographics 2025 conditional diffusion for an illumination effect | Background for specialized generation, not an acceleration kernel for this model |

ML2Code is an architectural precedent for generating specialized inference shaders and fusions. This survey did not establish a current official public general-purpose ML2Code compiler distribution to install. SDK artifacts and community mirrors alone do not establish a turnkey compiler or compatible model importer.

## Evidence levels for other rendering POCs

[dlss5-neural-amd](https://github.com/zmodelerlover/dlss5-neural-amd/tree/6410e56f57476344e0325e9d5ea34e9b9c229be3) explicitly wraps a separate HIP neural binary rather than implementing the network in its own source. Study its interop, timing and composition. The repository does not supply the underlying HIP kernels to transplant.

[RadeonNR](https://github.com/Yaddz/RadeonNR/tree/855b10355ba7b4bd46bee4748cfd0a75abfaabcf) also exposes integration code. Its [flow source](https://github.com/Yaddz/RadeonNR/blob/855b10355ba7b4bd46bee4748cfd0a75abfaabcf/src/neural/neural.cpp#L462) and [async UI caveat](https://github.com/Yaddz/RadeonNR/blob/855b10355ba7b4bd46bee4748cfd0a75abfaabcf/src/neural/neural.cpp#L5279) make it unsuitable as evidence of a proven fast same-frame implementation of our graph. README roadmap claims need separate verification.

[ZLUDA](https://github.com/vosen/ZLUDA) and [CUDA-for-AMD-Windows](https://github.com/Speedstu/CUDA-for-AMD-Windows) are compatibility research paths. Successful training or unrelated CUDA workloads do not establish support for the upstream Vulkan CUDA-launch extension, its FP8 PTX arithmetic, or this model's latency. Prioritize source-visible native kernels first.

## Ranked implementation experiments

The ranking below is an inference from our local profile and the cited source review. It is not a measured speedup forecast.

| Order | Experiment | Local code | Required evidence |
| --- | --- | --- | --- |
| 1 | Inspect actual attention LDS, VGPRs, spills and WMMA instruction delivery | `portable_window_body.glsl`, `amd_window.comp`, `vk_context.cpp` | RGA/RGP or executable statistics from the actual Windows pipeline; validation and driver pin |
| 2 | Distribute softmax work and publish weights with all 128 invocations | `portable_window_body.glsl` | Preserve fixed half tree, learned prior order, physical keys and shifted tails; same-backend bytes |
| 3 | Compact or alias attention scratch by phase; use fragment epilogues where supported | `portable_window_body.glsl` | Compiled LDS reduction and timing, with no overlapping live data or unverified fragment mapping |
| 4 | Packed K32/K64 staging and wider N tiles | `portable_gemm_body.glsl`, `kernels.cpp` | Two or four K16 operations with original publications; compare 64x16, 64x32, 64x64 resources/timings |
| 5 | Fuse the high-resolution C32 block and QKV normalization/attention | `nr_graph.cpp`, `kernels.cpp`, new AMD shaders | Remove large intermediate round trips; preserve an unfused capture route and compare complete blocks |
| 6 | Fuse FFN expansion, activation and contraction; specialize larger channel stages | Same graph and new shader variants | Keep quantization and split-K order; measure weight reuse and register pressure per shape |
| 7 | Compare a relaxed arithmetic fused variant with the same-model external implementation | Explicit experimental variant | Actual scene-linear SDR/HDR and temporal comparisons, no bit-exact label |
| 8 | Compare one operator in HIP/rocWMMA or D3D12 LinAlg | Separate prototype | Exact device/tool versions; include sharing/synchronization cost before judging backend benefit |

Specific findings behind the first experiments:

- The attention source and existing SPIR-V declare 34,816 bytes of Workgroup arrays, while the local device reports a 32,768-byte maximum. This is a declaration-level discrepancy; a working pipeline is not a compiled LDS or validation report. Determine actual allocation and legality, then reduce declarations explicitly. Do not infer occupancy from source size alone.
- The softmax section currently gives only invocations 0 through 63 an entire row each, including 64 serial weight publications. A parallel rewrite can preserve the rounded expression tree and coalesce publication without adopting generic online softmax.
- Every accelerated matrix K16 step still calls the scalar, bitwise `roundF16` helper. Explore precise hardware half operations or verified packed conversion sequences separately. Inspect machine code and test ties, subnormals, overflow, signed zero and nonfinite publication. The external POC demonstrates that simple narrowing/widening can disappear.
- GEMM currently stages a 64x16 output tile with two barriers per K16, and reloads input for every 16-column output group. Larger staged loads and wider tiles can improve reuse while retaining arithmetic; larger tiles may instead spill or lower occupancy.
- Global attention is already a small part of the measured target. It should not lead the next optimization round. The exact adapter/head can stay as useful anchors while the large FP8 stages are improved.

Whole-frame buffering, frame generation, skipped NR updates and old-result reprojection can affect displayed frame rate or latency, but do not establish faster evaluation of the current frame's full network. Treat them as separate behavior experiments if considered later. Keep the current single-pass pre-FSR resolution when comparing kernel changes.

## Measurement and promotion criteria

Use the existing commands and validation tools in [AMD.md](AMD.md), [amd-numerics.md](amd-numerics.md), `scripts/validate_model.ps1` and `tools/analyze_performance.py`. For each candidate:

1. Save model, executable and shader hashes, driver/compiler versions, dimensions, backend and every active switch. Keep the current accelerated and exact reference routes reproducible.
2. Test the operator and all applicable modes: residuals, activation, dual half/FP8 outputs, broadcasts, split-K, zero rows, padding and tails. Compare all 75 real-model boundaries and the head at 320x320 before large runs. Changed arithmetic uses a separately named result and quality criteria.
3. Measure warmed complete inference on an otherwise idle GPU at 1707x960 and a common 1920x1080 comparison size. Use sufficiently many frames and repeated runs to report median, p95 and variance; the current three-frame record is a starting reference, not a precision benchmark.
4. Keep per-dispatch minima for localization, but also export full family totals and actual per-frame samples. The existing profile truncates its kernel table and adds minima from different frames.
5. Compare matched scene-linear output, HDR highlights, motion, disocclusion, cuts and exposure transitions with the actual history and controls. Existing accelerated HDR/temporal gates remain unmet; source optimizations do not erase that requirement.
6. Confirm complete game frame times with the same Cyberpunk settings and frame generation off. Include bridge, FSR, GPU residency and synchronization. Capture/readback runs are separate from performance runs.

Promote a preservation-oriented optimization only after regression and target timing evidence agree. A different arithmetic policy or model needs its own quality decision. The immediate implementation target is a compact wave32 attention kernel followed by a fused C32 Vulkan path, with the external same-model shaders as a benchmark and design reference.
