# Native-half graph quality screen

The private ordinary7 native-half graph experiment fails the composed-frame
quality gate. All three generated SDR cases pass; all three HDR-highlight cases
fail. The experiment remains unqualified and is not integrated. Shipping
defaults, the exact reference and existing releases remain unchanged.

The [scalar evidence](performance/native-half-ordinary7-graph-quality-rx9070xt-20261004.json)
binds the private source closure, executable, loaded modules, run manifests,
contemporary logs and independent CPU audits. This is a separate follow-up to
the [operator arithmetic screen](amd-native-half-arithmetic-screen.md); the
earlier operator record is preserved as its original scope.

The complete network executes at 320x320 with native-half expansion only in
blocks1-4 and67-69. Each selected expansion uses C32 input at160x160, K32,
N128, FLAGS24, P0, B1 and N16/stage16. Seven loader-derived W1 buffers are
GPU-decoded once during initialization, adding57,344 bytes. Every selected
expansion decodes its complete padded input on the GPU, then uses the frozen
F16-input/F16-accumulator/F16-result shader. Two K16 operations remain ordered;
corrected mixed3 SiLU and the original terminal E4 publisher remain unchanged.
Contraction, residual, QKV and attention retain the original Pair/Arena policy;
exact head and history operators remain unchanged. QKV normalization, all fusion, packed publication and
tuning are off.

Both runs use the same private CLI, model, game shaders, source images, previous
histories, features and controls. The loaded eight-module baseline aggregate
is917ff3...; adding production9fad... and integer decoder1bef... produces the
ten-module candidate aggregate5d0742.... The generic arithmetic log still says
k16 for Pair families; explicit native-half policy fields and module hashes
identify the changed expansion arithmetic. The source captures intermediates
with the same production module. Production/intermediate capture
correspondence was not run after this quality rejection.

| Generated case | PSNR dB | SSIM | Gate |
| --- | ---: | ---: | --- |
| SDR reset | 47.079918 | 0.99811285 | Pass |
| SDR temporal | 43.479493 | 0.99406992 | Pass |
| SDR camera reset | 47.079918 | 0.99811285 | Pass |
| HDR reset | 30.887558 | 0.98826932 | Reject |
| HDR temporal | 34.981825 | 0.98649495 | Reject |
| HDR camera reset | 30.887558 | 0.98826932 | Reject |

Every matched frame must satisfy PSNR>=40 dB and SSIM>=0.99. Metrics use
unclamped scene-linear composed RGB and fixed data range1.0, including HDR
values above17. No display transform, per-frame range normalization or clipping
is applied. SSIM uses11x11 Gaussian windows with sigma1.5, population moments,
K1=.01/K2=.03, a channel mean and valid windows excluding the5-pixel border.
Independent float64 recomputation uses the opposite separable-filter pass
order and agrees with every recorded metric. The clamped modelcheck proxy is
not used for this quality screen.

The audit reopens96 raw/PFM/control/history files, verifies manifest hashes,
matches PFM RGB bits to raw RGBA, checks finite outputs and exact alpha, and
confirms identical inputs and controls. All18 fresh baseline head, RGBA and
published-history files match the earlier immutable preserving baseline byte
for byte. Camera-reset output and features reproduce the first reset in both
runs. Manifests and logs do not explicitly record whether validation layers
were enabled; this establishes no validation-layer coverage.

These are six generated rows from two source scenes with prescribed shared
histories. Camera-reset repeats the reset result. Temporal cases use identical
supplied histories, rather than independently evolved histories or a recorded
motion sequence. This does not establish natural SDR, gameplay motion, flicker,
ghosting, display HDR, original NVIDIA parity or target-resolution quality.

Both inference commands exit0 because the diagnostics execute successfully;
the separate quality comparator rejects the candidate. No75-checkpoint capture,
target-resolution native graph, lifecycle, timing, bridge or gameplay follow-up
was run after rejection. GPU decoding and its barriers would have to count in
any future inference measurement. There is no performance or promotion claim.
Private raw buffers, weights, images, ISA and executables remain excluded from
the repository and release packages.
