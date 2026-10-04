# Native-half matrix arithmetic screen

The private native-half experiment differs from the frozen raw-FP8 Pair
implementation. All seven model-generated target expansion inputs have E4
output differences. The GPU operand decoder, candidate production/capture E4
twins and original F24/F40 E4 anchors are exact. This is an arithmetic
localization result; the experiment remains unqualified and is not integrated.

The [scalar evidence](performance/native-half-arithmetic-rx9070xt-20261003.json)
binds source, modules, executables, driver reports, run manifests, contemporary
logs and independent CPU audits. Private raw buffers and model weights remain
outside the repository and release packages. Shipping defaults, the exact
reference and existing alpha releases are unchanged.

The candidate first decodes raw E4 operands into F16 on the GPU, then executes
two ordered K16 nonsaturating F16-input/F16-accumulator/F16-result cooperative
matrix operations. The baseline uses FP8 operands, F32 matrix results and an
FP16 publication after each operation. Corrected mixed3 SiLU and the original
terminal E4 publisher remain unchanged. The candidate is not a drop-in FP8
module; its operand storage and matrix arithmetic differ.

The fixed operator is ordinary C32 expansion: K32, N128, Nmatrix128, FLAGS24,
P0, B1, N16/stage16, local128 with required full wave32. Runs use the RX9070XT
1002:7550 and AMD26.9.1 (LLPC), with explicit upload, compute, transfer and host
read dependencies. Required device/driver, matrix tuple, memory/grid and raw
positive-zero seed checks run before pipelines. Validation layers are recorded
disabled with zero errors; this is not validation-layer coverage.

The independent decoder oracle checks all256 raw E4 codes. Finite E4 values
are represented exactly in F16; signed zeros are retained and raw7f/ff NaNs
map to signed canonical F16 NaNs7e00/fe00. Original model weight NaN cleanup
still happens before decoding. Each run includes20 decoder-only offset,
zero-count, tail, extraXYZ and canary cases, followed by comparisons of every
complete padded candidate input and W1 allocation. Every decoder comparison
is exact before native-half arithmetic executes.

| Run | Fixtures | Full allocation comparisons | Differing comparisons | Pair/native E4 differences | Activated-half diagnostic differences |
| --- | ---: | ---: | ---: | ---: | ---: |
| Bounded small | 52 | 332 | 67 | 27 | 40 |
| Bounded target additions | 54 | 344 | 71 | 29 | 42 |
| Seven model-generated target inputs | 7 | 62 | 14 | 7 | 7 |

The bounded runs use synthetic activations, including model-weight cases. The
separate target replay uses seven captured model-generated ordinary inputs for
blocks1-4 and67-69 at R414720/C32, from a1707x960 graph padded to1728x960. These
are internal864x480 feature tensors, not game captures. Inputs, controls, model
weights, source closure and original E4 ancestry are pinned. Independent audits
reopen every raw allocation, verify byte differences and padding canaries, and
repack the model weights. The seven original packed E4 outputs also match the
previous V12 baseline. No arithmetic differences are waived.

| Block | Differing E4 bytes | Differing activated-half diagnostic bytes |
| --- | ---: | ---: |
| 1 | 401,708 | 24,594,759 |
| 2 | 450,072 | 22,677,675 |
| 3 | 848,541 | 23,916,863 |
| 4 | 846,827 | 25,719,627 |
| 67 | 911,550 | 15,637,452 |
| 68 | 953,428 | 17,926,217 |
| 69 | 1,085,329 | 18,765,473 |

The target replay compares2,176,522,400 bytes across124 saved raw files and
checks265,423,040 canary bytes. All14 differences belong to the baseline/native
E4 and activated-half diagnostic pairs. The20 decoder-only cases, seven decoded
A allocations, seven decoded W1 allocations, seven candidate production/capture
E4 twins and seven original F24/F40 E4 anchors are exact. All three inference
runs return exit1 because the recorded arithmetic differences remain visible.

The F40 activated-half trace is a separate diagnostic; it cannot establish the
uninstrumented original F24 pre-E4 half bits. Feature tensor PSNR or SSIM is
not composed-frame image quality. No scene-linear RGB quality gate, temporal
sequence, whole native-half graph, timing, bridge timing or game FPS result is
claimed. A future arithmetic experiment must satisfy composed-frame quality
gates and include GPU input decoding/barriers in its performance measurement.

Installed driver reports show two F16 WMMA instructions in each native matrix
module and no FP8 WMMA there. Native production uses24 VGPRs,26 SGPRs and4096
bytes LDS; capture uses24/30/4096. Both have zero scratch, three vector plus one
scalar F32-to-F16 cast and twelve vector F16-to-F32 casts. The integer decoder
uses9/14/0 with zero scratch and no matrix, floating conversion or MODE setter.
These are static installed-pipeline counts, not executed instruction counts,
occupancy or timing. Offline counts remain separately bound. The raw executable
subgroup property128 is preserved separately from the required matrix wave32.

The scalar record links the decoder contract and fresh source compilation,
bounded and actual host source reviews, bounded saved evidence and actual
saved evidence. Their CPU passes establish identity, addressing and observed
bytes within this fixed scope; they do not qualify arbitrary arithmetic,
NVIDIA parity, whole-model quality or a production default.
