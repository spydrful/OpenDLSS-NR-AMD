# Documentation

Choose a starting point based on what you want to do.

## Install and use the alpha

| Guide | What you'll find |
| --- | --- |
| [Installation](INSTALL.md) | Requirements, download checks, model import, controls, upgrades, removal and troubleshooting |
| [Alpha 6 release notes](releases/v0.1.0-alpha.6.md) | What changed, which files to download and current limits |
| [All downloads](https://github.com/spydrful/OpenDLSS-NR-AMD/releases) | Published packages and their checksums |
| [Advanced use](ADVANCED.md) | Optional Pair/Arena and C32 settings, diagnostic tools, benchmarks and captures |

## Understand or develop the AMD runtime

Start with the [technical guide](TECHNICAL.md). It explains the frame path and
terms before walking through a source build.

| Reference | What you'll find |
| --- | --- |
| [Development evidence](DEVELOPMENT.md) | Detailed project status, measurements and build notes retained from the original README |
| [AMD setup reference](AMD.md) | Full Windows build, patched host and packaging instructions |
| [AMD arithmetic](amd-numerics.md) | Exact reference versus accelerated arithmetic, device requirements and precision limits |
| [Game runtime API](../game/README.md) | DirectX 12 resources, shared buffers/fences, frame ownership, cancellation and resets |
| [Current performance work](amd-performance-next-steps.md) | Bottlenecks, rejected experiments and remaining validation |
| [Performance research](amd-performance-research.md) | External kernel research and proposed experiments |
| [Validation record](rx9070xt-validation.md) | Historical model, driver and game measurements with their limitations |
| [Static dependency sources](../integrations/optiscaler/sources/README.md) | Source pins, patches and library build limitations |
| [Release writing template](../.github/RELEASE_TEMPLATE.md) | A consistent format for future release pages |

Measured results belong to their named build and settings. Network-only timings
exclude the game, bridge and FSR; use a complete game benchmark for FPS claims.

## Original network design

The [original design overview](UPSTREAM-DESIGN.md) preserves the network's data
flow, representations and invariants. These notes describe the upstream NVIDIA
implementation. Its PTX scheduling, display composition and parity results have
their own scope; use the AMD guides above for the current game runtime.

| Reference | Topic |
| --- | --- |
| [Network](network.md) | Blocks, resolution pyramid, padding and windows |
| [Numerics](numerics.md) | Publication points, rounding and exactness contract |
| [Weights](weights.md) | Model directory and packed layouts |
| [Execution](execution.md) | Original Vulkan/PTX resources, kernels and scheduling |
| [Frame](frame.md) | Original demo preprocessing, composition and temporal history |
| [Upstream README](upstream-nvidia.md) | Preserved NVIDIA build instructions and measurements |
