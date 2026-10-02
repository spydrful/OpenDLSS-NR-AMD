# Install the RX 9070 XT alpha

This guide installs the prebuilt **v0.1.0-alpha.1** package in Cyberpunk 2077 on
Windows. This is an experimental development release: at 1440p output with FSR
Quality, enabled NR measured **4.56 / 4.56 / 4.57 FPS** and about **213 ms for NR
plus its bridge**. Performance and game-quality gates remain unmet. NR ships
**disabled**; enable it deliberately for testing.

## Requirements

- **Radeon RX 9070 XT**, Windows 11 and an AMD Vulkan driver exposing the required
  RDNA4 FP8 matrices and wave32 execution. Adrenalin **26.9.1** is the tested
  driver; other GPUs and driver versions are not validated by this release.
- An installed copy of **Cyberpunk 2077**. Version **2.31**, native DirectX 12,
  is the tested game. Start with 2560 x 1440 output, High graphics fields,
  FSR Quality, ray tracing off and frame generation off. Disable driver AFMF
  when measuring rendered game performance.
- [PowerShell 7](https://learn.microsoft.com/en-us/powershell/scripting/install/installing-powershell-on-windows)
  and the current [Microsoft Visual C++ v14 Redistributable **x64**](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
  Installing the prebuilt package does not require Visual Studio or a Vulkan SDK.
- Your own supported **nvngx_dlssnr.dll**, imported locally. NVIDIA DLLs and
  extracted model weights are **not included** in the download or repository.

## Download and check the package

1. Open the [v0.1.0-alpha.1 release](https://github.com/spydrful/OpenDLSS-NR-AMD/releases/tag/v0.1.0-alpha.1).
   Download **OpenNR-AMD-v0.1.0-alpha.1-rx9070xt.zip** and its **.zip.sha256** asset.
   GitHub's automatic **Source code** archives do not contain the built DLLs.
2. Compare the ZIP's SHA-256 with the sidecar, then extract it to a writable
   folder, for example `D:\OpenNR-AMD-alpha`. Keep this package for removal.
3. Open **PowerShell 7** (`pwsh`) in the extracted package's root: the folder
   containing `package-manifest.json`, `payload`, `scripts` and `tools`.

For example, in the download folder:

```powershell
Get-FileHash './OpenNR-AMD-v0.1.0-alpha.1-rx9070xt.zip' -Algorithm SHA256
Get-Content './OpenNR-AMD-v0.1.0-alpha.1-rx9070xt.zip.sha256'
Expand-Archive './OpenNR-AMD-v0.1.0-alpha.1-rx9070xt.zip' -DestinationPath 'D:\OpenNR-AMD-alpha'
```

The installer also validates the package's managed payload hashes. Do not edit
`payload/OptiScaler.ini` or other payload files before installation.

## Install and import your model

Close Cyberpunk first. Set `$game` to the folder containing
**Cyberpunk2077.exe**, normally the game's **bin\x64** folder. The following
example uses the GOG installation path; replace it for another installation.

```powershell
$game = 'C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64'

./scripts/install.ps1 -PackageDirectory . -GameDirectory $game -WhatIf
./scripts/install.ps1 -PackageDirectory . -GameDirectory $game

./scripts/import_model.ps1 -NvidiaDll 'D:\local\nvngx_dlssnr.dll' -Destination "$game\open-nr\model"
```

Replace the `-NvidiaDll` path with your local DLL. Write permission to the game
folder is required; run that PowerShell window as administrator if the folder's
permissions deny the install or import. The installer uses `dxgi.dll` as its
default OptiScaler proxy and backs up existing managed files before replacing
them. An existing OpenNR install must be removed before installing another one.

The importer reads PE/resource bytes without loading or executing the DLL.
It accepts only these pinned containers and validates the model resource and
required tensors before publishing output:

| Supported container | Complete DLL SHA-256 |
| --- | --- |
| DLSS-NR 310.8.0 | `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` |
| DLSS-NR 310.8.SF.0 (numeric file version 310.8.2.0) | `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927` |

Both contain the same supported `WEIGHTS_HT` resource. Other versions are
rejected. The destination must be **new**: the importer does not overwrite an
existing model. If a previous installation left a verified model, reuse it;
otherwise preserve the old directory elsewhere before importing again.

After import, these paths should exist:

```text
<game>\bin\x64\open-nr\model\manifest.json
<game>\bin\x64\open-nr\model\model\stage0.bin
...
<game>\bin\x64\open-nr\model\model\stage10.bin
```

Do not copy the NVIDIA DLL into the game or share the DLL or extracted files.

## Enable and use NR

Launch Cyberpunk with FSR Quality enabled. NR is initially **off**. Press
**Insert**, open the **Neural** tab, and select **Enable NR** when ready to test.
The configured `NrBackend=mochizuki` is the host's compatibility name for loading
this package's `OpenNrRuntime.dll`; leave it as packaged.

The OpenDLSS-NR AMD panel exposes **Effect strength**, **Colour strength**,
**Highlight guard**, **Style**, **Use motion and history**, **History strength**,
**Status**, **Render input** and **NR + bridge** timings. Defaults run one
accelerated pass at the full internal render resolution, with temporal history
enabled. The initial target input is 1707 x 960, padded to 1728 x 960.

Use **Enable NR** to turn the network off and return to ordinary FSR.
Setting effect strength to zero still runs inference. The displayed NR timing
is not game FPS. Broad motion/face/ghosting review and corrected game capture
validation remain pending; scene-linear precision failures are also documented
in the [validation record](https://github.com/spydrful/OpenDLSS-NR-AMD/blob/v0.1.0-alpha.1/docs/rx9070xt-validation.md).

## Remove or upgrade

Close Cyberpunk. From the package root, use the same `$game` path:

```powershell
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

Removal restores verified original managed files and preserves separately
imported models and unlisted user files. Keep the `.open-nr-install.json` ledger
and `.open-nr-backup-*` directory intact until removal completes. To upgrade,
remove the old installation using its package, then install the new one.

If **Save Settings** in the overlay changed `OptiScaler.ini`, removal will stop
before changing game files. Preserve your edited INI outside the game, restore
the packaged INI from the **same installed release**, then retry:

```powershell
$iniBackup = Join-Path (Get-Location) ('OptiScaler.user-backup-' + [Guid]::NewGuid().ToString('N') + '.ini')
Copy-Item "$game\OptiScaler.ini" $iniBackup
Copy-Item './payload/OptiScaler.ini' "$game\OptiScaler.ini" -Force
./scripts/uninstall.ps1 -GameDirectory $game -WhatIf
./scripts/uninstall.ps1 -GameDirectory $game
```

The generated backup filename preserves your edits in the package folder. For
any other changed managed file, preserve or move it before retrying; do not edit the ledger
or delete original backups to bypass the check.

## Troubleshooting and reports

| Symptom | Next step |
| --- | --- |
| Very low FPS with NR enabled | Expected in this alpha at the target resolution; clear **Enable NR**. |
| Missing `MSVCP140` / `VCRUNTIME140` dependency | Install the current Microsoft Visual C++ v14 **x64** Redistributable linked above. |
| Model import rejected | Check the complete DLL hash above and choose a new destination; unsupported containers are rejected. |
| No Insert overlay | Confirm installation targeted the folder containing `Cyberpunk2077.exe`; inspect existing proxy/mod conflicts and the host log. |
| NR unavailable or bypassed | Check model paths and **Status**; run the capability query below and inspect the host log in the game folder. |
| Installer reports a changed payload | Extract an unchanged copy of the same release; do not modify the packaged payload. |
| Removal reports a changed managed file | Preserve it outside the game and follow the restoration instructions above. |

An optional capability query from the package root is:

```powershell
./tools/dlss5vk.exe info --backend amd --interop
```

Report the release tag, GPU, driver, Windows/game versions, render/output
resolution, enabled controls, status, timings and relevant error text in a
[GitHub issue](https://github.com/spydrful/OpenDLSS-NR-AMD/issues).
Keep proprietary DLLs, model files and game captures out of issue attachments.
The package includes corresponding source, build scripts and component license
notices; see its `source` directory and `NOTICE`.
