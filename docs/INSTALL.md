# Install OpenNR-AMD for Cyberpunk 2077

This guide is for **v0.1.0-alpha.6**, a development alpha for **RX 9070 XT on
Windows 11**. Use the guide from the same release as your download.

> **Set expectations:** NR is off after installation. Enable it deliberately
> when you are ready to test, and expect a substantial performance cost.
> Alpha 6 game FPS has not been measured; performance and broad game-quality
> targets remain unmet. The reported alpha 6 improvement measures the network
> alone, not game FPS. See [the evidence and limits](ADVANCED.md#current-evidence-and-limits).

[Check requirements](#before-you-start) · [Download](#1-download-the-built-package) ·
[Install](#3-install-in-the-game-folder) · [Import a model](#4-import-your-local-model) ·
[Enable NR](#5-launch-and-enable-nr) · [Remove or update](#remove-update-or-roll-back) ·
[Troubleshooting](#troubleshooting-and-reports)

## Before you start

| You need | Tested setup / notes |
| --- | --- |
| **Radeon RX 9070 XT** | AMD Vulkan driver with the required RDNA4 FP8 matrices and wave32 execution. Adrenalin **26.9.1** is tested; other GPUs and drivers are not validated by this release. |
| **Windows 11** | The native Windows package is the tested platform. |
| **Cyberpunk 2077** | Version **2.31**, native DirectX 12. Start at **2560 × 1440 output**, High graphics fields, **FSR Quality**, ray tracing off and frame generation off. Disable driver AFMF when measuring rendered game performance. |
| **PowerShell 7** | Install [PowerShell 7](https://learn.microsoft.com/en-us/powershell/scripting/install/installing-powershell-on-windows); open the **PowerShell 7** app, also called `pwsh`. |
| **Microsoft Visual C++ v14 runtime** | Install the current [**x64 Redistributable**](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). |
| **Your own supported `nvngx_dlssnr.dll`** | Import it locally in step 4. NVIDIA DLLs and extracted model weights are **not included** in the package or repository. Only the two exact containers listed below are accepted. |

The prebuilt package does **not** require Visual Studio or a Vulkan SDK.
Close Cyberpunk before installing, removing or updating.

## 1. Download the built package

Open the [v0.1.0-alpha.6 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.6)
and download these two assets:

- **OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip** — the installable package.
- **OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip.sha256** — its checksum.

GitHub's automatic **Source code (zip)** and **Source code (tar.gz)** downloads
do not contain the built DLLs.

If you are using an older package, use its matching guide:
[alpha 5](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.5/docs/INSTALL.md),
[alpha 4](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.4/docs/INSTALL.md),
[alpha 3](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.3/docs/INSTALL.md).
Their downloads remain available unchanged:
[alpha 5 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.5),
[alpha 4 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.4),
[alpha 3 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.3).

## 2. Check and extract the ZIP

Open PowerShell 7 in your download folder. Run the first two commands and
compare the ZIP hash with the hash in the `.sha256` file. They must match.
Then extract to a new writable folder; change the destination if needed.

```powershell
Get-FileHash './OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip' -Algorithm SHA256
Get-Content './OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip.sha256'
Expand-Archive './OpenNR-AMD-v0.1.0-alpha.6-rx9070xt.zip' -DestinationPath 'D:\OpenNR-AMD-alpha'
Set-Location 'D:\OpenNR-AMD-alpha'
```

You should now be in the package root: it contains `package-manifest.json`,
`payload`, `scripts` and `tools`. Run the remaining commands from this folder.
Keep the package for removal or rollback.

The installer checks the managed payload hashes too. Leave
`payload/OptiScaler.ini` and the other payload files unchanged before installing.

## 3. Install in the game folder

Find the folder containing **Cyberpunk2077.exe** — usually the game's
**bin\x64** folder. Set `$game` to that full path. The example below is a GOG
installation; replace it with your actual Steam, GOG or other installation path.

```powershell
$game = 'C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64'
Test-Path -LiteralPath "$game\Cyberpunk2077.exe"
```

The check should return **True**. Preview the installation, then run it:

```powershell
./scripts/install.ps1 -PackageDirectory . -GameDirectory $game -WhatIf
./scripts/install.ps1 -PackageDirectory . -GameDirectory $game
```

`-WhatIf` previews the changes without installing. The installer uses
`dxgi.dll` as the default OptiScaler proxy and backs up existing managed files
before replacing them. Keep the generated `.open-nr-install.json` ledger and
`.open-nr-backup-*` directory in the game folder for removal.

If an OpenNR install already exists, [remove it first](#remove-update-or-roll-back).
If Windows denies write access to the game folder, reopen **PowerShell 7 as
administrator**, return to the package root and set `$game` again before retrying.

## 4. Import your local model

Replace the DLL path below with your supported local `nvngx_dlssnr.dll`.
The destination must be **new**; the importer does not overwrite an existing model.
If a previous installation left a verified model at this location, reuse it
and skip this command. Otherwise preserve the old directory elsewhere before
importing again.

```powershell
./scripts/import_model.ps1 -NvidiaDll 'D:\local\nvngx_dlssnr.dll' -Destination "$game\open-nr\model"
```

The importer reads PE/resource bytes without loading or executing the DLL.
It validates the model resource and required tensors before publishing output.
Only these exact complete DLL hashes are supported:

| Supported container | Complete DLL SHA-256 |
| --- | --- |
| DLSS-NR 310.8.0 | `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` |
| DLSS-NR 310.8.SF.0 (numeric file version 310.8.2.0) | `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927` |

Both contain the same supported `WEIGHTS_HT` resource. Other versions are
rejected. To check your DLL before importing:

```powershell
Get-FileHash 'D:\local\nvngx_dlssnr.dll' -Algorithm SHA256
```

After import, these paths should exist (`$game` already includes `bin\x64`):

```text
<game folder>\open-nr\model\manifest.json
<game folder>\open-nr\model\model\stage0.bin
...
<game folder>\open-nr\model\model\stage10.bin
```

Do not copy the NVIDIA DLL into the game or share the DLL or extracted files.
Import requires write permission to the destination, just like installation.

## 5. Launch and enable NR

1. Launch Cyberpunk and use the starting graphics settings above, including
   **FSR Quality**.
2. Press **Insert** to open the overlay, then open the **Neural** tab.
3. Check **Enable NR** when you are ready to test. NR starts **off**.
4. Clear **Enable NR** whenever you want to return to ordinary FSR.

Setting **Effect strength** to zero still runs inference. Turn off **Enable NR**
to stop the network and recover its performance cost.

The OpenDLSS-NR AMD panel exposes **Effect strength**, **Colour strength**,
**Highlight guard**, **Style**, **Use motion and history**, **History strength**,
**Status**, **Render input** and **NR + bridge** timings. Defaults use one
accelerated pass at the full internal render resolution, with temporal history
on. The starting input is 1707 × 960, padded to 1728 × 960. The displayed NR
timing is not game FPS.

Keep `NrBackend=mochizuki` and `[Spoofing] StreamlineSpoofing=false` as packaged.
These are required compatibility settings for this FSR-based integration;
[the advanced guide explains them](ADVANCED.md#host-configuration-and-controls).

## Optional Pair/Arena selection

For your first launch, keep the normal auto selection and packaged defaults.
Installing alpha 6 does **not** activate Pair/Arena or C32 QKV normalization;
QKV normalization defaults to off. To test them deliberately, follow the
[process-only opt-in recipe](ADVANCED.md#optional-pairarena-selection).
It selects kernels and still requires you to enable NR in the overlay.

## Remove, update or roll back

Close Cyberpunk. Open PowerShell 7 in the **installed version's package root**,
set `$game` to the same executable folder, and preview removal before running it:

```powershell
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

Removal restores verified original managed files and preserves separately
imported models and unlisted user files. Keep the `.open-nr-install.json` ledger
and `.open-nr-backup-*` directory intact until removal completes.

To **update**, remove the old installation using its retained package, then
follow this guide with the new package. To **roll back**, remove the current
installation, install the older retained package and follow that release's guide.
A verified imported model can be reused.

### If removal stops because settings changed

**Save Settings** in the overlay can change `OptiScaler.ini`. Removal then stops
before changing game files. Preserve your edited INI outside the game, restore
the packaged INI from the **same installed release**, then retry:

```powershell
$iniBackup = Join-Path (Get-Location) ('OptiScaler.user-backup-' + [Guid]::NewGuid().ToString('N') + '.ini')
Copy-Item "$game\OptiScaler.ini" $iniBackup
Copy-Item './payload/OptiScaler.ini' "$game\OptiScaler.ini" -Force
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

The generated backup filename preserves your edits in the package folder.
For any other changed managed file, preserve or move it before retrying. Do not
edit the ledger or delete original backups to bypass the check.

## Troubleshooting and reports

| Symptom | Next step |
| --- | --- |
| Very low FPS with NR enabled | Clear **Enable NR**. Alpha 6 game FPS is unmeasured; historical alpha 3 NR-on averaged 10.69 FPS. The gameplay budget remains unmet. |
| Missing `MSVCP140` / `VCRUNTIME140` dependency | Install the current Microsoft Visual C++ v14 **x64** Redistributable linked above. |
| Model import rejected | Check the complete DLL hash in step 4 and use a new destination; unsupported containers are rejected. |
| `Destination already exists` during import | Reuse a previously verified model, or preserve the old directory elsewhere before importing to a new one. |
| No Insert overlay | Confirm the install folder contains `Cyberpunk2077.exe`; inspect existing proxy/mod conflicts and the host log in the game folder. |
| NR unavailable or bypassed | Check the model paths and **Status**; run the capability query below and inspect `open-nr/runtime.log` and the host log in the game folder. |
| Installer reports a changed payload | Extract an unchanged copy of the same release; leave the packaged payload unchanged. |
| Installer reports an existing install ledger | Remove the previous installation with its retained package before installing again. |
| Removal reports a changed managed file | Preserve it outside the game and follow the restoration instructions above. |
| Access denied during install or import | Run PowerShell 7 as administrator, return to the package root and set `$game` again. |

Optional capability query, run from the package root:

```powershell
./tools/dlss5vk.exe info --backend amd --interop
```

For a [GitHub issue](https://github.com/spydrful/OpenDLSS-NR-AMD/issues), include
the release tag, GPU, driver, Windows/game versions, render/output resolution,
enabled controls, status, timings and relevant error text. Keep proprietary
DLLs, model files and game captures out of issue attachments.

For kernel controls, reproducible network benchmarks and bounded captures,
continue to [Advanced testing and diagnostics](ADVANCED.md). The package also
includes corresponding source, build scripts and component license notices;
see its `source` directory and `NOTICE`.
