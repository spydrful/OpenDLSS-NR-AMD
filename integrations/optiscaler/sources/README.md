# Static dependency sources

These intact upstream source archives include the implementation, build files
and original notices for the five static dependency families identified by the
pinned OptiScaler project and headers. The archive hashes are checked before packaging and
recorded in the package manifest. They are copied intact so unusual build-file
extensions and additional license texts are preserved.

| Dependency | Official archive | Length | SHA-256 | Upstream commit |
| --- | --- | --- | --- | --- |
| FreeType 2.13.3 | [freetype-2.13.3.tar.xz](https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz) | 2,617,564 bytes | `0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289` | `42608f77f20749dd6ddc9e0536788eaad70ea4b5` |
| Microsoft Detours 4.0.1 | [detours-4.0.1.tar.gz](https://codeload.github.com/microsoft/Detours/tar.gz/refs/tags/v4.0.1) | 346,614 bytes | `75df7f1f084acaf83c3b96a90328e93823c2506a97363d50556f1ee700c81f68` | `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833` |
| OptiScaler FSR2 DX11 2.2.1 | [fsr2-dx11-2.2.1.tar.gz](https://codeload.github.com/optiscaler/FidelityFX-FSR2-DX11/tar.gz/f2e3f86390746eb3f0bd1b28e91ea3cbc790ee76) | 27,012,684 bytes | `5afbba69e4cce4a5bbfb571f3b7be7ed66327862795a0f2a51c9f5deb639eb7c` | `f2e3f86390746eb3f0bd1b28e91ea3cbc790ee76` |
| OptiScaler FSR2 Fsr212 2.1.2 | [fsr2-212-2.1.2.tar.gz](https://codeload.github.com/optiscaler/FidelityFX-FSR2-212/tar.gz/ce46c0fa359a1bdc4280d9550d0df011e0e6dd49) | 26,472,580 bytes | `044b54719dca93af7abaf37674c98ba2eb993d257d09663e399e2e76f582281f` | `ce46c0fa359a1bdc4280d9550d0df011e0e6dd49` |
| OptiScaler FSR3 DX11 3.1.2 | [fsr3-dx11-3.1.2.tar.gz](https://codeload.github.com/optiscaler/FidelityFX-SDK-DX11/tar.gz/8138c9dc086154706643a03def91f3d01d391cd0) | 136,489,331 bytes | `0cf311b4095d0e9781b241b1ebaf205bb5768cce2a673220bbfcc1e76d0f9a52` | `8138c9dc086154706643a03def91f3d01d391cd0` |

FreeType's official [download page](https://freetype.org/download.html) identifies
the release server. The official mirror's
[VER-2-13-3 tag](https://github.com/freetype/freetype/tree/VER-2-13-3) has annotated
tag object `534ad3456055ee1f65ecde3bcf22a656a31514d1`, resolving to the commit
above. Detours' official [v4.0.1 tag](https://github.com/microsoft/Detours/tree/v4.0.1)
points directly to its listed commit.

Run `scripts/fetch_static_sources.ps1` from the repository to retrieve missing
archives. Existing archives must match both length and hash; the fetcher refuses
to overwrite different files. The large archives are ignored by Git and explicitly
included by `scripts/package.ps1` in the final corresponding-source bundle.

Use `tar -xf <archive>` to extract into a separate development directory.
Follow FreeType's `docs/INSTALL*` or Detours' `README.md` and makefiles. FSR2
2.2.1 uses `build/BuildLibs.bat` for DX11/DX12/Vulkan libraries. FSR212's
`build/BuildLibs.bat` builds DX12/Vulkan and names its outputs
`ffx_fsr2_212_api_*`. The FSR3 snapshot's `sdk/BuildFidelityFXSDK.bat` and
`sdk/BuildFidelityFXSDKSolution.bat` enable the DX11 backend and shader
compilation. Keep the authored `build/` directories; they are preserved inside
the intact archives even though application packaging prunes generated trees.

Before rebuilding the FSR3 DX11 backend, run:

```powershell
.\scripts\patch_fsr31_static_source.ps1 -SourceDirectory <extracted FidelityFX-SDK-DX11 root>
```

This checked helper applies [fsr31-local-symbols.patch](fsr31-local-symbols.patch):
it renames `ffxGetDeviceDX11` and `ffxGetResourceDX11` to their `_Fsr31` forms
in the backend header and implementation. Those names match the local host's
declarations and avoid collisions with the other static backend. The helper
checks both original source hashes before any change, verifies patched hashes,
and permits an already-patched rerun. No supplied library is overwritten.

Source selection evidence is version/interface correspondence. FSR221 core
matches after comments, formatting and export qualifiers are normalized;
FSR212 core/interface match with its `Fsr212` namespace and version macros;
FSR31 core/interface match after formatting, include paths, export qualifiers
and the local header's `Fsr31` namespace wrapper are normalized. The C linkage
declarations preserve the symbol ABI; the two DX11 symbol renames are the
additional documented source change. Header wrappers remain in the supplied
host source. These comparisons do not reconstruct original compiler settings
or prove that the pinned source commits produced the existing `.lib` bytes.

The current OptiScaler build still links the supplied
`external/freetype/freetype.lib`, `OptiScaler/library/detours/detours.lib`, and
the `OptiScaler/library/fsr2`, `fsr2_212` and `fsr31` library families.
Header version declarations are the basis for selecting these source versions.
The original libraries' compiler, options, enabled modules, dependent libraries
and any other local modifications were not recorded in the pinned host repository.
These snapshots do not establish byte reproduction of those prebuilt libraries
and do not change the frozen game binaries.

The earlier fresh-source host build relinked these provided libraries. Windows,
D3DX, Vulkan-loader and XeSS SDK import libraries are system/vendor inputs.
Their provided headers/notices are retained where present. The source archives
also preserve upstream compiler tooling and licensed sample assets; they do
not contain local model weights or game captures and are not installed into
the game. Original upstream notices remain in each archive; copies of the
principal licenses accompany the package.
