# OpenDLSS-NR-AMD

**Experimental neural rendering for AMD Radeon on Windows.**

OpenDLSS-NR-AMD runs the OpenDLSS-NR neural-rendering network in DirectX 12 games
using a native AMD backend. It processes the game's rendered image before FSR
upscales it, with controls for the effect, colour and temporal history.
The first tested setup is **Cyberpunk 2077 on a Radeon RX 9070 XT**.

> [!IMPORTANT]
> This is a **development alpha for testing**. Performance and broad visual-quality
> checks are still incomplete, and enabling NR can substantially reduce frame rate.
> NR is **off by default**. Alpha 6 game FPS has not been measured.

**[Download Alpha 6](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.6)**
 · **[Installation guide](docs/INSTALL.md)**
 · **[Technical guide](docs/TECHNICAL.md)**
 · **[All releases](https://github.com/spydrful/OpenDLSS-NR-AMD/releases)**

## Before you start

| You need | Tested setup |
| --- | --- |
| Graphics card | **AMD Radeon RX 9070 XT**; the only currently validated GPU |
| Operating system | **Windows 11**, native DirectX 12 |
| Game | **Cyberpunk 2077 2.31** |
| Driver | **AMD Adrenalin 26.9.1**, with the required Vulkan FP8 support |
| Install tools | **PowerShell 7** and the **Visual C++ v14 Redistributable x64** |
| Model | Your own supported **nvngx_dlssnr.dll**, imported locally |

Start with **2560 × 1440 output, FSR Quality, ray tracing and frame generation
off**, and an SDR display. The tested graphics settings use High fields with the
configuration labeled Custom. Other GPUs, games, Linux, ray tracing and HDR
display support still need validation.

NVIDIA DLLs and extracted model weights are **not included**. The
[installation guide](docs/INSTALL.md) lists the two accepted DLL versions and
their hashes. Installing the prebuilt package does not require a compiler.

## Get started

1. Open the **[Alpha 6 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.6)**.
   Under **Assets**, download `OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip` and
   `OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip.sha256`.
   GitHub's **Source code** archives contain source files; choose the named ZIP
   for the built application.
2. Follow the **[installation guide](docs/INSTALL.md)** to check the download,
   install the prerequisites and extract the package. Open **PowerShell 7** in
   the extracted folder containing `package-manifest.json`.
3. Close the game. Set the paths below to your game's **bin\x64** folder and your
   supported model DLL, then run:

```powershell
$game = 'C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64'
$modelDll = 'D:\local\nvngx_dlssnr.dll'

./scripts/install.ps1 -PackageDirectory . -GameDirectory $game -WhatIf
./scripts/install.ps1 -PackageDirectory . -GameDirectory $game
./scripts/import_model.ps1 -NvidiaDll $modelDll -Destination "$game\open-nr\model"
```

`-WhatIf` previews the installation. You need write permission to the game
folder. The model destination must be new; an existing verified model can be
reused. If you already have OpenNR installed, follow the
[upgrade instructions](docs/INSTALL.md#remove-update-or-roll-back) first.

4. Launch the game with **FSR Quality**. Press **Insert → Neural → Enable NR**
   when you are ready to test. Use the same checkbox to turn it off again;
   setting effect strength to zero still runs the network.

Keep the extracted package and installation backups for removal or rollback.
The [installation guide](docs/INSTALL.md) covers controls, logs and common problems.

## What's new in Alpha 6?

Alpha 6 adds an optional optimization that combines two steps of the network.
In the measured setup, it reduced **network processing time by about 5.29%**:

| Measurement | Optimization off | Optimization on |
| --- | ---: | ---: |
| Median network GPU time | 50.41 ms | 47.75 ms |

This compares the same Alpha 6 build with explicit Pair/Arena settings at
1707 × 960 input, padded to 1728 × 960. It excludes the game, the D3D12/Vulkan
bridge and FSR. **It is not a game FPS result.** The optimization is optional;
installing Alpha 6 keeps the existing automatic selection and leaves it off.
See the [release notes](docs/releases/v0.1.0-alpha.6.md) for the full comparison
and the [advanced guide](docs/ADVANCED.md#optional-pairarena-selection) to opt in.

## What should I expect?

**Is it ready for everyday gameplay?** It is currently suited to development and
experimentation. The 8 ms NR-plus-bridge and 60 real FPS targets remain unmet.
Broader testing of motion, faces, ghosting and highlights is still pending.

**Will it work on another Radeon or in another game?** The RX 9070 XT and
Cyberpunk 2077 are the initial validated target. RX 7000 acceleration and wider
game support are future work.

**How does it fit with FSR?** NR processes the image at the game's internal
render resolution. FSR then upscales the result to the display resolution.

**How do I undo the installation?** Close the game and use the retained package:

```powershell
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

Removal verifies managed files and restores backed-up originals. If you saved
overlay settings or changed installed files, follow the
[removal instructions](docs/INSTALL.md#remove-update-or-roll-back) before retrying.

**How do I report a problem?** Open an
[issue](https://github.com/spydrful/OpenDLSS-NR-AMD/issues) with the release version,
GPU, driver, game version, settings and steps to reproduce it. Include relevant
error text from `open-nr/runtime.log` and the host log in the game's `bin\x64`
folder. Check logs for personal paths before sharing them; keep model DLLs and
weights local.

## Technical details

The AMD runtime uses Vulkan compute and RDNA4 FP8 matrix kernels. A patched
OptiScaler host shares GPU buffers and fences with DirectX 12, so the neural pass
runs on the same GPU without per-frame CPU image transfers.

The [technical guide](docs/TECHNICAL.md) explains the frame path, terminology,
build steps and what the benchmarks establish. From there:

- **[Advanced use](docs/ADVANCED.md)** — optional kernel settings, measurements and captures.
- **[Development evidence](docs/DEVELOPMENT.md)** — detailed measurements, validation and build reference.
- **[Remaining work](docs/amd-performance-next-steps.md)** — performance priorities and outstanding checks.
- **[Documentation index](docs/README.md)** — runtime API, arithmetic and original network design.

## Credits and licenses

Based on [OpenDLSS-NR](https://github.com/maanHimself/OpenDLSS-NR), with host
integration adapted from
[neural-amd-opti](https://github.com/MatheusFerreiraS/neural-amd-opti/tree/557bb8553098395f5f138c2e22ed25f256f7a3a2)
and model importing adapted from
[DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD/blob/82560c4fbfaac347fc5e22c22025191402ae916b/windows/package/model-tools/dlssnr_extract_model.cpp).

The neural core and importer use **MIT** licenses. The derived game runtime and
OptiScaler adapter use **GPL-3.0-or-later**; binary packages include corresponding
source and build scripts. See [LICENSE](LICENSE), [NOTICE](NOTICE) and the
[component credits](docs/DEVELOPMENT.md#project-and-licenses).
This project is not affiliated with AMD or NVIDIA.
