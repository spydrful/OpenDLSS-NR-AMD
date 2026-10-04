# Windows runtime interface

`open_nr_api.h` is a versioned C interface. Call `OpenNrGetApi(1, ...)` with a
correct `struct_size`, then query the actual D3D12 adapter's capabilities. The
query tests imported shared buffers and timeline fences on the same adapter
LUID. Supported arithmetic is E4M3 16×16×16 with FP32 matrix accumulation and
FP16 publication, executing in wave32. Capability success is not a game quality
or performance result.

Alpha 6 adds independent C32 QKV projection/normalization behind the process
selection `DLSS5VK_AMD_QKV_NORMALIZE=off|c32`, read once at session creation.
It defaults to `off`; NR and the existing qualified auto cache keep their
defaults. `c32` requires explicit optimized Pair/Arena, K16/N16/stage16/Q32,
every other fusion/hardware-publication override off and an empty tuning path.
See the [process-only launch example](../docs/INSTALL.md#optional-pairarena-selection).
The route covers only blocks 0–4/66–70 and is independent of the older
QKV/attention fusion flag. Its shared bridge and ordered publications are
capability-, model- and module-checked; intermediate capture decomposes the
same selected arithmetic. Logs and captures record `qkvNormalize` separately.
The version 1 C ABI, frame-slot ownership and D3D12 queue/fence ordering are
unchanged. The [delivery record](../docs/amd-qkv-normalize-delivery.md) distinguishes
ordinary network timings from native lifecycle and game performance evidence.

Create a session on a direct queue from the supplied D3D12 device and prepare
its local assets. Per frame, set metadata, prepare, record inputs, record
outputs, then submit the producer. At the safe submission boundary call
`EnqueueHip` (the OptiScaler compatibility spelling of Vulkan inference),
submit the consumer, and retire. Recording outputs after enqueue is also
supported. `Record*` functions only record; they never execute a game list.

Color is scene-linear RGBA16F, RGBA32F, or R11G11B10F. Motion is RG16F/RG32F or
RGBA16F/RGBA32F, in NGX/FFX current-to-previous pixels after applying its scales.
Metadata declares valid rectangle origins, current render-pixel jitter (zero
when motion already includes jitter), exposure texture state, exposure scale,
and pre-exposure. Current exposure is sampled by the GPU. Output is scene-linear
RGBA16F in the original exposure domain; it has no final display transform.
Values outside the representable half-float range cannot be preserved in that
output format. Typeless, multisampled, inaccessible, or foreign-device resources
are declined. Unsupported or busy frames retain normal FSR and reset NR history.

Frame IDs must be consecutive for temporal reuse. Reset after a camera cut or
discontinuity. Missing motion uses spatial inference and primes history. Resize
waits for consumers at a drain boundary. Submission must use the same direct
queue COM identity as session creation; another queue on the same device is
rejected before fence signals and cannot enter compatibility fallback.
Submit prepared jobs in increasing preparation order. A canceled preparation
may leave a gap; submitting an older job after a newer one is rejected before
timeline signals. Discard its recorded lists before cancellation.
Session queue changes require the host
to drain and recreate the session. The runtime has eight lazily allocated slots,
each with its own descriptor pool. Busy slots cause a bypass rather than
unbounded allocations. Prepared frames carry their predecessor's jitter;
submission resolves history against the frame actually submitted before them.
Cancellation, skipped frames and resets invalidate that ancestry.

The compatibility ABI's separate detail strength, debug views, residual
smoothing, optional preprocessing, display-encoded input, later-pass overrides,
and dynamic-resolution buckets are unsupported. Nondefault requests are
declined. Use model intensity for effect strength. The patched host exposes
the supported controls, sends scene-linear input with exact render extents,
and disables the compatibility smoother.

Cancel only a job whose recorded commands were discarded or whose D3D12 work
has been proved complete by a queue fence, with no Vulkan work submitted. The
normal host cancellation path uses list discard; the failed-record path first
waits for the actual queue. Cancellation of submitted or retired neural work
is rejected. Drain rejects an unretired recorded
continuation, then waits for GPU completion before resource destruction.
On a failed wait, the runtime preserves live resources. A failure before Vulkan
submission leaves the GPU output marker clear, causing the consumer to use
original scene color; a failure after submission cannot be assumed recoverable.

The optional `OpenNrRecoverSubmission` export is a failure-only between-submit
operation for the patched host. With one isolated pending job, it waits for
prior consumers and Vulkan work, then records a marker clear on the actual
D3D12 queue before the continuation. A successful recovery supplies original
color and still requires consumer submission, retirement and draining. The
session then requires recreation. A partial-record failure may continue through
ordinary FSR without recording an NR output; its recovered job still retains
all producer resources until the game continuation is submitted and retired.
Recovery resources remain alive until the
consumer fence completes. Failed recovery keeps the job, session and runtime
DLL pinned. Normal frames do not use this wait or perform image readback.

Runtime compute recording changes heaps, root bindings, and pipeline state.
The patched OptiScaler host records on its real producer/consumer segments and
restores captured game bindings afterward. Other hosts must provide the same
continuation-state preservation and safe producer/consumer ordering themselves.
Input resource states are restored by the runtime.

`GetTimings` reads completed GPU timestamp metadata, never image data. It reports
pack, preprocess, network, composite, and unpack times. `nr_bridge_ms` measures
elapsed GPU time from pack start to unpack end on the actual D3D12 queue,
including the Vulkan handoff; it falls back to the sum of stages if the D3D12
timestamp frequency is unavailable. This is not game FPS. Allocation statistics
cover graph activations and history, excluding weights, shared buffers, FSR,
the game, driver padding, and other allocations. Use measured process VRAM for
release reports.

The optional `MochizukiNrGetInfo` compatibility export supplies limited menu
telemetry. Its `frames` field is the resettable temporal seed count, and its
history percentage currently reports whether history has been primed, rather
than the fraction of recent frames that actually consumed history. It does
not populate the compatibility failure/error or network-dispatch fields.
Its median and P95 cover NR plus the bridge, as labeled by the patched menu.
Use `OpenNrGetTimings` for actual submission/bypass counters and completed
stage timings. Compatibility history telemetry is not evidence of temporal
correctness or an image-quality validation result.

For local profiling, create an empty `open-nr/record-timings.flag` before
session creation. Completed jobs append GPU timestamps and counters to
`open-nr/gpu-timings.csv`. DXGI process-local video memory is sampled every
60 completed NR jobs; `-1` means the query was unavailable. This includes
the game's allocations when loaded in the game, but excludes other processes
and system-wide VRAM use. Rows describe completed NR jobs, not game presents.
Measure game frame intervals separately with PresentMon and frame generation
off. Remove the flag to disable this trace on the next session. Tracing is
disabled in the package by default.

The wrapper and pinned OptiScaler integration are GPL-3.0-or-later. The neural
core retains its MIT license. See `integrations/optiscaler/LICENSE` and `NOTICE`.
