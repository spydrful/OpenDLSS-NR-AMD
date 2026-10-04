# How OpenNR AMD works

This guide explains the native AMD path and gives you a starting point for a
source build. To use the prebuilt alpha, follow the [installation guide](INSTALL.md).
For all benchmark details and historical records, see
[development evidence](DEVELOPMENT.md).

## The path from a game frame to the screen

```text
Cyberpunk 2077 renders a DirectX 12 frame
    ↓
Patched OptiScaler host prepares the frame and motion data
    ↓  shared GPU buffers and fences
AMD Vulkan runtime preprocesses, runs NR and composes the result
    ↓
D3D12 bridge supplies the result texture to the host
    ↓
FSR upscales it to the display resolution
```

The DirectX 12 game and Vulkan runtime use the same GPU. Shared buffers carry
the data; fences tell each API when the other has finished. Normal frames stay
on the GPU. Image readback is an optional diagnostic step.

The network processes the game's internal resolution. The initial test uses
**1707 × 960 input for 2560 × 1440 output with FSR Quality**. The model pads its
working image to **1728 × 960** so its windows and resolution levels fit.

Temporal history carries information from the previous processed frame. Motion
vectors reproject that history onto the current frame. History resets after
skipped NR frames or discontinuities. Unsupported inputs or unsafe boundaries
bypass NR and preserve ordinary FSR where safe. The
[runtime API](../game/README.md) describes resource ownership and failure handling.

## Terms you'll see in the technical notes

| Term | Meaning here |
| --- | --- |
| **NR** | Neural rendering: the network processes the rendered image before FSR |
| **FP8 / E4M3** | An 8-bit floating-point format used for many network values and matrix operations |
| **FP16 / half** | A 16-bit floating-point format used at specified intermediate boundaries |
| **Kernel** | A GPU program that computes one part of the network |
| **GEMM** | Matrix multiplication, a major part of the network's processing cost |
| **QKV / attention** | Query, key and value projections used by transformer blocks to combine information |
| **K16 publication** | The retained arithmetic policy publishes intermediate matrix results at ordered 16-element K steps |
| **Pair / Arena** | Optional matrix and attention implementations with their own validated settings |
| **C32 QKV normalization** | Alpha 6's optional combined projection/normalization route for selected 32-channel blocks |
| **Qualified auto cache** | Tested kernel choices tied to the GPU, driver, model, shaders and geometry |
| **Median / P95 / P99** | Typical time and the times below which 95% / 99% of measured samples fall |

Low precision makes rounding order part of the result. A faster implementation
must preserve the intended intermediate boundaries as well as stay within memory
limits. See [AMD arithmetic](amd-numerics.md) for the full contract.

## Read the measurements correctly

Alpha 6 compares its C32 QKV normalization route **off versus on in the same
build**, with explicit Pair/Arena, K16/N16/stage16/Q32 and other experiments off.
At the target input size, median network time is **50.41260 → 47.74616 ms**, a
**5.28923% improvement**. Three interleaved pairs of 30 measured frames after
five warmups retain 90 samples per condition. Image readback and dispatch
instrumentation are off.

That measurement covers **network GPU time only**. It excludes the DirectX
12/Vulkan bridge, FSR, game and presentation. Alpha 6 has no measured complete
game FPS, bridge time or game VRAM result. The **8 ms NR-plus-bridge** and
**16.67 ms complete-frame / 60 real FPS** targets remain unmet.

The new route is **off by default**. The existing qualified auto cache stays in
place; you must select Pair/Arena and C32 explicitly to reproduce this result.
Use the [advanced recipe](ADVANCED.md#optional-pairarena-selection) for the exact
process settings and [delivery record](amd-qkv-normalize-delivery.md) for evidence.

Preservation checks establish equality to the tested AMD baseline within their
scope. They do not establish original NVIDIA parity, broader game quality or
HDR display validation. Existing highlight errors against the exact reference
remain unresolved. Historical game FPS in older records belongs to those older
builds.

## Build from source on Windows

Use **PowerShell 7**, **Visual Studio 2022 C++ tools**, a **Windows SDK** and **Git**.
Windows 11 and the RX 9070 XT are the validated target. GPU execution requires
an AMD Vulkan driver exposing the required RDNA4 FP8 matrices and wave32 execution.
Python 3.10+ is used by measurement tools; NumPy is needed for SSIM image comparisons.

Clone the repository, then open PowerShell 7 in its root:

```powershell
git clone https://github.com/spydrful/OpenDLSS-NR-AMD.git
cd OpenDLSS-NR-AMD

./scripts/fetch_tools.ps1
./scripts/build.ps1 -Backend amd
./scripts/build_game.ps1 -SkipCore
./scripts/build_importer.ps1 -Test
./scripts/build_optiscaler.ps1 -Fetch
```

These build the core, game runtime, model importer and patched host. Query the
device and run the numerical checks:

```powershell
./build/dlss5vk.exe info --backend amd --interop
./build/dlss5vk.exe selftest --backend reference
./build/dlss5vk.exe selftest --backend amd --amd-kernels optimized
```

The AMD selftest selects `optimized` explicitly because its synthetic inputs do
not match the model identity in the auto cache. A capability query alone does
not prove that shared resources work. Before game testing, run the controlled
bridge and host checks:

```powershell
./scripts/build_interop.ps1 -Run
./scripts/test_host_safety.ps1 -RunGpu
```

For a local package, fetch the corresponding dependency source and package the
built host:

```powershell
./scripts/fetch_static_sources.ps1
./scripts/package.ps1 -OptiScalerDll './build/optiscaler/OptiScaler.dll' -OutputDirectory './dist/local-development'
```

The output directory must be new. Source builds create development packages;
they do not reproduce a frozen release unless you use its corresponding source,
tools and recorded build settings. Follow [AMD setup](AMD.md) and
[development evidence](DEVELOPMENT.md#build-on-windows) for the detailed options.

## Where to go next

- [Advanced use](ADVANCED.md) for selectors, benchmark commands and bounded captures.
- [Current performance work](amd-performance-next-steps.md) for measured bottlenecks and proposed experiments.
- [Game runtime API](../game/README.md) for integration work.
- [Original network design](UPSTREAM-DESIGN.md) for the model's structure and upstream arithmetic.
- [Documentation index](README.md) for the remaining references.
