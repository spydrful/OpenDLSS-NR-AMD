# RX 9070 XT Pair/Arena delivery

The repaired Pair GEMM and Arena attention route reduces ordinary target
network median from **58.684860 to 50.500780 ms**, a **13.9458%** improvement
against the frozen published alpha 4 Direct-RTE/Register-RTE policy. Alpha 5
ships these modules as **explicit opt-ins**. The existing
qualified auto cache, K16 arithmetic defaults and NR-disabled setting remain
unchanged. The **8 ms NR-plus-bridge** and **16.67 ms / 60 real FPS** targets
remain unmet; new complete-game FPS, bridge timings and game VRAM are unmeasured.

The [scalar evidence](performance/pair-arena-rx9070xt-20261003.json) binds current
source, binary, shader, model and driver identities. The
[alpha 4 publication identities](performance/alpha4-release-identities.json),
[historical RTE delivery](amd-rte-delivery.md) and
[experiment erratum](amd-performance-experiments.md#expanded-suite-erratum)
retain the earlier evidence. No earlier release, asset or measurement is revised.

## Measured scope

Windows 11, RX 9070 XT (`1002:7550`), Adrenalin **26.9.1 (LLPC)** and Vulkan
**1.4.349** are the tested environment. Both conditions use one full-resolution
pass at valid **1707×960**, padded to **1728×960**, K16 publication,
N16/stage16/Q32, with every fusion and packed-publication experiment off.

| Ordinary network GPU timing | Frozen alpha 4 | Repaired Pair/Arena Q32 |
| --- | ---: | ---: |
| Median | 58.684860 ms | 50.500780 ms |
| P95 | 58.922124 ms | 50.689466 ms |
| P99 | 59.053322 ms | 50.815118 ms |
| Mean | 58.662694 ms | 50.471625 ms |
| Standard deviation | 0.208596 ms | 0.165297 ms |
| Coefficient of variation | 0.3556% | 0.3275% |

Three interleaved pairs each use five warmup frames and 30 measured frames,
retaining **90 samples per condition**. Image readback and dispatch
instrumentation are off. Pooled and each paired median/P95 pass the ≤2%
regression gate; the median improvement exceeds the 5% performance threshold.
These timestamps exclude the D3D12 bridge, FSR, game and presentation and
establish no game FPS.

Separate instrumented profiles retain actual frame samples. Medians of
per-frame family sums are **38.932560 → 32.151100 ms FP8 GEMM** and
**7.983360 → 6.479880 ms window attention**. Twenty-five of 46 GEMM groups and
all 22 attention groups meet the 5% operator improvement gate. Independent
dispatch minima and profile totals do not replace ordinary timing.

The external-comparison run uses valid **1920×1080**, padded to **1920×1152**,
and the same 90-sample protocol. Ordinary median is **78.576060 → 68.383780 ms**,
P95 **78.956024 → 68.760702 ms**, and P99 **79.028253 → 68.861261 ms**. It adds
network-only geometry timing and no 1080p quality or game validation claim.

## Preserving implementation and erratum

`amd_gemm_direct_rte_pair` preloads two original K16 FP8 operand fragments when
K >128, then executes each matrix operation, half publication and partition
update in the original order. Smaller K retains the preceding loop. The route
currently supports **N16/stage16** only. It includes the mixed SiLU helper:
three scalar half-RTE publications, with original software publication at the
inner F32 FMA and final multiply. The exhaustive-half-pattern and additional
F32-witness diagnostic remains independently recorded in the experiment notes.

The expanded suite found **10,477 differing bytes** in a valid
K192/N48/63-row/flags-0/partition-96/two-batch case in historical Init/Epilogue
and initial Pair. The repaired modules retain the original shared-memory
initializer for every partitioned operator; runtime-zero initialization applies
only to non-residual, unpartitioned K ≤512. Earlier 880-check experimental
reports remain historical and cannot qualify the repaired modules. Current
proof tools require the executed paired-preload regression set for all three.
The published alpha 4 Direct-RTE module is unaffected.

`amd_window_arena_rte` reuses one packed shared-memory arena for half scores,
E4 weights and half output scratch, with explicit phase barriers. It preserves
learned priors, physical key order, ordered K16 products/publications, exponent
approximation, eight-lane softmax tree, shifted-window masks, padded keys and E4
saturation. Declared per-workgroup storage falls from Register-RTE's
**9,728 / 13,312 bytes** to **6,656 / 9,216 bytes** for Q16/Q32. The device's
32 KiB limit is a per-workgroup limit and establishes no occupancy claim.

Installed-driver inspection of the actual target Q32 attention pipeline reports
**9,216 LDS bytes, 38 VGPRs, 29 SGPRs and zero scratch bytes**, with six static
FP8 matrix instruction sites. All 32 FP8 GEMM specializations contain matrix
instructions; their reported range is 32–45 VGPRs, 29–46 SGPRs, 4,096 LDS bytes
and zero scratch. These are static instruction/resource observations. Spill
counts, utilization and resident workgroups are not established. Pipelines
request wave32; the driver's executable `subgroup 128` field does not establish
wave128 execution. Offline RGA is separate and no RGP capture is claimed.

Pair requires compatible half RTE/denormal/signed-zero controls and F32
signed-zero/Inf/NaN preservation. A structural postcompile closure retains both
16- and 32-bit signed-zero modes. Arena requires the half controls and enumerated
FP16 accumulator matrix type, supports Q16/Q32, and rejects Q64. Unsupported
forced choices fail visibly. The portable reference remains unchanged.

## Completed strict and lifecycle scope

- Four actual policies pass **724 operators / 934 checks / 15,264,768 bytes
  each**: Init/Arena Q32, Epilogue/Arena Q32, Pair/Arena Q16 and Pair/Arena Q32.
  The expanded cases cover paired boundaries, partitions, eight batches, tails,
  broadcasts, residuals, activation, conversion edges and thin-window padding.
- Pair/Arena Q32 formal decomposed model proofs pass **77 checks at 320×320**:
  all 75 boundaries, F32 head and capture/production reproduction. The target
  proof passes **two checks** for head and capture reproduction. It does not
  export all 75 target boundaries. Composition proxies do not establish
  unclamped scene-linear game quality.
- Six synthetic SDR/HDR reset, temporal and camera-reset cases exactly match
  the prior AMD K16 route. Absolute-reference highlight failures remain
  unresolved. Separately screened K32/final accumulation fails all three HDR
  cases; neither arithmetic experiment is promoted.
- Eight existing genuine SDR game frames pass **80 raw byte checks /
  2,107,883,520 bytes**, plus **16 exact composed RGB comparisons** using
  unclamped scene-linear RGB and data range 1.0. Identical history uses captured
  ancestry; evolved history starts from reset and uses each variant's own
  outputs. False original-capture flags in evolved replay are expected and
  are distinct from the exact inter-variant comparison. No new scene or visual
  review is added.
- The actual runtime DLL passes ABI/lifecycle, cancellation, resize, reset,
  recovery, drain/shutdown, shared-buffer/fence and command-list continuation
  checks, with eight sequential requested frames at 320×320 and target geometry.
  The separate **source-included production-pool harness** additionally verifies
  eight distinct live prepared target slots before submission. Ordered outputs
  match across eight frames/**104,878,080 bytes**; cancel/gap/reset outputs match
  across seven frames/**91,768,320 bytes**. These are controlled readback tests,
  preserving queue order; they establish neither concurrent inference nor game
  VRAM/performance. The Vulkan validation layer was absent.

Broad motion, faces, disocclusion, exposure, flicker/ghosting review, HDR display,
full scene-linear highlight acceptance, three complete warmed Cyberpunk benchmark
pairs and ten minutes of active gameplay remain outstanding. Original NVIDIA
parity, RX 7000 acceleration, other games/platforms and ray tracing are unclaimed.

## Reproduce or opt in

Build current source into a new directory with the Windows prerequisites in
[AMD.md](AMD.md#build-and-local-checks). Supply your own imported model and
extract the unchanged alpha 4 package separately:

```powershell
$candidate = 'D:\amd\build\experiments\pair-arena'
$alpha4 = 'D:\local\OpenNR-AMD-alpha4'
$model = 'D:\local\open-nr\model'
./scripts/build.ps1 -Backend amd -OutputDirectory $candidate
./scripts/build_game.ps1 -SkipCore -OutputDirectory $candidate
$policy = @('--amd-kernels','optimized','--amd-arithmetic','k16',
  '--amd-gemm','direct-rte-pair','--amd-tile-n','16','--amd-stage-k','16',
  '--amd-window-layout','arena-rte','--amd-window-queries','32',
  '--amd-fusion','0','--amd-ffn32-fusion','0','--amd-qkv32-fusion','0',
  '--amd-expert-fusion','0','--amd-block-fusion','0','--amd-hardware-publication','0')
& "$candidate\dlss5vk.exe" amdcheck --backend amd --comparison-anchor rte32 `
  --baseline-shaders "$alpha4\payload\open-nr\shaders" --shaders "$candidate\shaders" `
  --fixture 'D:\amd\build\experiments\pair-arena-operators' @policy
./scripts/benchmark_amd.ps1 -Executable "$candidate\dlss5vk.exe" `
  -ShaderDirectory "$candidate\shaders" -BaselineExecutable "$alpha4\tools\dlss5vk.exe" `
  -BaselineShaderDirectory "$alpha4\payload\open-nr\shaders" -ModelDirectory $model `
  -OutputDirectory 'D:\amd\build\experiments\pair-arena-bench' -ComparisonAnchor rte32 `
  -Kernels optimized -Arithmetic k16 -Gemm direct-rte-pair -TileN 16 -StageK 16 `
  -WindowLayout arena-rte -WindowQueries 32 -Ffn32Fusion 0 -Qkv32Fusion 0 `
  -Width 1707 -Height 960 -Warmup 5 -Frames 30 -Pairs 3
```

Use new absent outputs and freeze actual identities. Repeat `-Mode profile`
separately; captures/profiles must not run during ordinary measurements. The
`rte32` anchor retains Direct-RTE/N16/stage16/Register-RTE/Q32/K16 and all
experiments off; immutable comparisons additionally bind the alpha 4 release
identities. [Experiment commands](amd-performance-experiments.md#reproduce-the-development-route)
cover decomposed model capture and formal qualification; substitute the new
Pair/Arena policy. Generated tuning stays private and does not replace the
packaged qualified cache.

The [installation guide](INSTALL.md#optional-pairarena-selection) provides
process-only game selection and reversal. NR remains disabled until enabled
deliberately. Alpha 5 includes the original MIT core/importer notices, GPL host
corresponding source and dependency notices. NVIDIA DLLs, extracted weights,
model buffers and game captures remain excluded. Final package/publication
identities are recorded when delivery completes; rebuilt EXE/DLL byte identity
is not claimed.
