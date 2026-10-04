$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts\install_common.ps1')
$taskScratch = Join-Path $taskRoot ('build\package-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskScratch | Out-Null
$taskChecks = 0
function Assert-Nr([bool]$Condition, [string]$Message) {
  ++$script:taskChecks
  if (-not $Condition) { throw $Message }
}
function Assert-NrThrows([scriptblock]$Action, [string]$Message) {
  ++$script:taskChecks
  $threw = $false
  try { & $Action } catch { $threw = $true }
  if (-not $threw) { throw $Message }
}
try {
  $taskHost = Join-Path $taskScratch 'host'
  $taskKernels = Join-Path $taskScratch 'kernels'
  $taskGameShaders = Join-Path $taskScratch 'game-shaders'
  $taskOutput = Join-Path $taskScratch 'result'
  New-Item -ItemType Directory -Path $taskHost, $taskKernels, $taskGameShaders | Out-Null
  $taskFfxRoot = Join-Path $taskHost 'external\FidelityFX-SDK-v2'
  $taskAgilityRoot = Join-Path $taskHost 'external\directx_agility_sdk'
  New-Item -ItemType Directory -Path (Join-Path $taskFfxRoot 'Kits\FidelityFX\signedbin'), (Join-Path $taskFfxRoot 'docs'), (Join-Path $taskAgilityRoot 'lib') | Out-Null
  foreach ($taskName in @('amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_upscaler_dx12.dll')) {
    Set-Content -LiteralPath (Join-Path $taskFfxRoot ('Kits\FidelityFX\signedbin\' + $taskName)) -Value 'synthetic FFX dependency'
  }
  Set-Content -LiteralPath (Join-Path $taskAgilityRoot 'lib\D3D12Core.dll') -Value 'synthetic Agility dependency'
  Set-Content -LiteralPath (Join-Path $taskAgilityRoot 'LICENSE.txt') -Value 'synthetic Agility notice'
  Set-Content -LiteralPath (Join-Path $taskFfxRoot 'docs\license.md') -Value 'synthetic FFX notice'
  Set-Content -LiteralPath (Join-Path $taskFfxRoot '3rdpartynotice.md') -Value 'synthetic FFX third-party notice'
  Set-Content -LiteralPath (Join-Path $taskHost 'linker-input.lib') -Value 'synthetic tracked linker input'
  Copy-Item -LiteralPath (Join-Path $taskRoot 'integrations\optiscaler\LICENSE') -Destination (Join-Path $taskHost 'LICENSE')
  Set-Content -LiteralPath (Join-Path $taskHost 'mock.cpp') -Value '// synthetic GPL host source'
  New-Item -ItemType Directory -Path (Join-Path $taskHost 'OptiScaler\exports') | Out-Null
  Set-Content -LiteralPath (Join-Path $taskHost 'OptiScaler\exports\exports.h') -Value '// authored export header, not build output'
  Set-Content -LiteralPath (Join-Path $taskHost 'nvngx_dlssnr.dll') -Value 'must not be copied'
  Set-Content -LiteralPath (Join-Path $taskHost 'stage0.bin') -Value 'must not be copied'
  New-Item -ItemType Directory -Path (Join-Path $taskHost 'dist\streamline') | Out-Null
  Set-Content -LiteralPath (Join-Path $taskHost 'dist\streamline\reflex.license.txt') -Value 'synthetic preserved notice'
  Set-Content -LiteralPath (Join-Path $taskHost 'dist\streamline\sl.reflex.dll') -Value 'must not be copied'
  $taskRuntime = Join-Path $taskScratch 'runtime.dll'
  $taskProxy = Join-Path $taskScratch 'proxy.dll'
  $taskBridge = Join-Path $taskGameShaders 'bridge.hlsl'
  Set-Content -LiteralPath $taskRuntime -Value 'synthetic runtime'
  Set-Content -LiteralPath $taskProxy -Value 'synthetic OptiScaler'
  Set-Content -LiteralPath $taskBridge -Value '// synthetic pack/unpack source'
  Set-Content -LiteralPath (Join-Path $taskKernels 'kernel.spv') -Value 'synthetic SPIR-V'
  Set-Content -LiteralPath (Join-Path $taskKernels 'experimental.spv') -Value 'unapproved synthetic candidate'
  Set-Content -LiteralPath (Join-Path $taskGameShaders 'game_preprocess.spv') -Value 'synthetic game SPIR-V'
  $taskParams = @{
    RuntimeDll = $taskRuntime; OptiScalerDll = $taskProxy; OptiScalerSource = $taskHost
    ShaderDirectory = $taskKernels; ShaderFileNames = @('kernel.spv'); GameShaderDirectory = $taskGameShaders; BridgeShader = $taskBridge
    OutputDirectory = $taskOutput
  }
  & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams
  $taskManifest = Read-NrPackage $taskOutput
  Assert-Nr (@($taskManifest.files).Count -eq 12) 'expected payload files are missing'
  Assert-Nr ($taskManifest.releaseChannel -eq 'development' -and $null -eq $taskManifest.releaseTag -and $null -eq $taskManifest.sourceCommit) 'unversioned development package metadata changed'
  Assert-Nr ($taskManifest.nrEnabledByDefault -eq $false -and (Get-Content -LiteralPath (Join-Path $taskOutput 'payload\OptiScaler.ini') -Raw) -match '(?m)^Enabled=false\s*$') 'generated configuration must make NR opt-in'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskOutput 'payload\OptiScaler.ini') -Raw) -match '(?m)^\[Spoofing\]\r?\nStreamlineSpoofing=false\r?$') 'generated AMD configuration must disable NVIDIA Streamline capability spoofing'
  Assert-Nr ($taskManifest.amdArithmeticDefault -eq 'k16' -and $null -eq $taskManifest.amdTuning -and -not (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\open-nr\shaders\amd-tuning.json'))) 'absent optional tuning changed alpha arithmetic or entered payload'
  Assert-Nr ($taskManifest.amdQkvNormalizeDefault -eq 'off') 'C32 QKV normalization must remain opt-in'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskOutput 'PACKAGE-README.txt') -Raw) -match 'Insert\s*\r?\n?-> Neural -> Enable NR') 'package instructions do not explain how to enable NR'
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\open-nr\shaders\experimental.spv'))) 'unselected experimental shader entered payload'
  Assert-Nr (@($taskManifest.releaseGates.PSObject.Properties | Where-Object { $_.Value.status -ne 'unmet' }).Count -eq 0 -and @($taskManifest.releaseGates.PSObject.Properties).Count -eq 4) 'unmet numerical/game release gates missing'
  Assert-Nr ($taskManifest.releaseGates.gameTemporalHdr.displayScope -eq 'initial SDR' -and $taskManifest.releaseGates.gameTemporalHdr.hdrDisplayValidation -like 'deferred*') 'HDR display deferral is missing from the initial game gate'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'amd-numerics.md')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'rx9070xt-validation.md'))) 'linked package-root validation documents are missing'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'FreeType-FTL.TXT')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'FreeType-LICENSE.TXT'))) 'FreeType binary notices are missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\open-nr\shaders\bridge.hlsl')) 'bridge HLSL missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\open-nr\shaders\game_preprocess.spv')) 'game shader missing'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'tools\analyze_runtime.py')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'tools\analyze_presentmon.py')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'scripts\fetch_presentmon.ps1'))) 'diagnostic tools are missing from package'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'tools\dlss5vk.exe')) -and @($taskManifest.diagnosticTools).Count -eq 2 -and @($taskManifest.files | Where-Object { $_.destination -like '*.exe' }).Count -eq 0) 'diagnostic executable missing, unhashed or scheduled for game installation'
  Assert-Nr (@($taskManifest.diagnosticSources).Count -eq 11 -and @($taskManifest.diagnosticSources | Where-Object { $_.installedInGame }).Count -eq 0) 'diagnostic source metadata missing or scripts scheduled for game installation'
  foreach ($taskDiagnostic in $taskManifest.diagnosticSources) {
    Assert-Nr ((Get-NrHash (Get-NrChild $taskOutput $taskDiagnostic.file)) -eq $taskDiagnostic.sha256) 'diagnostic script/tool source missing or altered'
  }
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'tools\tune_amd.py')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'tools\qualify_amd_model.py')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'tools\compare_amd_scenes.py')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'scripts\analyze_amd_shaders.ps1')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'scripts\rga_tool_manifest.json'))) 'AMD qualification/offline analysis delivery tools missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\game\runtime.cpp')) 'runtime corresponding source missing'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\integrations\optiscaler\HostJobLifetime.h')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\integrations\optiscaler\recovery_tests.inc'))) 'host safety integration sources missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\scripts\test_host_safety.ps1')) 'host safety build/test driver missing'
  Assert-Nr (@($taskManifest.sourceDependencies).Count -eq 5 -and @($taskManifest.sourceDependencies | Where-Object { $_.byteReproductionEstablished }).Count -eq 0) 'static dependency source metadata missing or byte reproduction claimed'
  foreach ($taskDependency in $taskManifest.sourceDependencies) {
    Assert-Nr ((Get-NrHash (Get-NrChild $taskOutput $taskDependency.source)) -eq $taskDependency.sha256) 'pinned static dependency source archive missing or altered'
  }
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\integrations\optiscaler\sources\fsr31-local-symbols.patch')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\scripts\patch_fsr31_static_source.ps1'))) 'FSR31 local source changes missing'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\ports\browser-webgpu\tools\headless.mjs')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\ports\browser-webgpu\shaders\ops.wgsl'))) 'browser reference/diagnostic sources missing'
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\ports\browser-webgpu\web\fixtures'))) 'generated browser fixtures entered source package'
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\tools\rga'))) 'downloaded portable RGA tool entered corresponding application source'
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\tools\rgp'))) 'downloaded portable RGP/RDP tools entered corresponding application source'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\ports\browser-webgpu\web\vendor\basis\LICENSE')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\ports\browser-webgpu\web\vendor\basis\ATTRIBUTION.md'))) 'vendored Basis license and attribution missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\third_party\optiscaler-host\mock.cpp')) 'host source has wrong relative layout'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\third_party\optiscaler-host\OptiScaler\exports\exports.h')) 'authored host export header was pruned'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\third_party\optiscaler-host\dist\streamline\reflex.license.txt')) -and -not (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\third_party\optiscaler-host\dist\streamline\sl.reflex.dll'))) 'host distribution notices were pruned or NVIDIA DLLs were copied'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\OptiScaler\amd_fidelityfx_upscaler_dx12.dll')) 'FSR dependency is missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\OptiScaler\D3D12_OptiScaler\D3D12Core.dll')) 'Agility dependency is missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'source\OpenDLSS-NR-AMD\third_party\optiscaler-host\linker-input.lib')) 'tracked SDK linker input was discarded'
  Assert-Nr (-not (Get-ChildItem -LiteralPath (Join-Path $taskOutput 'source') -File -Recurse | Where-Object { $_.Name -in @('nvngx_dlssnr.dll','stage0.bin') })) 'model or NVIDIA binary entered package'
  $taskHash = Get-NrHash (Join-Path $taskOutput 'package-manifest.json')
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'existing package was overwritten'
  Assert-Nr ((Get-NrHash (Join-Path $taskOutput 'package-manifest.json')) -eq $taskHash) 'failed repackage changed output'
  $taskParams.OutputDirectory = Join-Path $taskScratch 'alpha-result'
  $taskParams.ReleaseTag = 'v0.1.0-alpha.99'
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'versioned alpha without a source commit was accepted'
  $taskParams.SourceCommit = 'HEAD'
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'symbolic source commit was accepted'
  $taskParams.SourceCommit = ('a' * 40) + "`n"
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'newline in source commit was accepted'
  $taskParams.SourceCommit = 'A' * 40
  $taskParams.ReleaseTag = "v0.1.0-alpha.99`n"
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'trailing newline in release tag was accepted'
  $taskParams.ReleaseTag = "v0.1.0-alpha.99`nother"
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'multiline release tag was accepted'
  Assert-Nr (-not (Test-Path -LiteralPath $taskParams.OutputDirectory)) 'invalid release metadata published output'
  $taskParams.ReleaseTag = 'v0.1.0-alpha.99'
  # Exercise the managed optional file without claiming that this synthetic
  # manifest is runtime-qualified. Runtime identity validation is a separate test.
  $taskTuningSource = Join-Path $taskKernels 'amd-tuning.json'
  Set-Content -LiteralPath $taskTuningSource -Value '{"format":"OpenNR-amd-tuning-v1","syntheticPackageContract":true}'
  & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams
  $taskAlphaManifest = Read-NrPackage $taskParams.OutputDirectory
  $taskTuningHash = Get-NrHash $taskTuningSource
  $taskTuningEntries = @($taskAlphaManifest.files | Where-Object { $_.destination -eq 'open-nr/shaders/amd-tuning.json' })
  Assert-Nr ($taskTuningEntries.Count -eq 1 -and $taskTuningEntries[0].role -eq 'amd-tuning' -and $taskTuningEntries[0].sha256 -eq $taskTuningHash) 'optional tuning is missing, duplicated or not managed by its hash'
  Assert-Nr ($taskAlphaManifest.amdTuning.file -eq 'payload/open-nr/shaders/amd-tuning.json' -and $taskAlphaManifest.amdTuning.sha256 -eq $taskTuningHash -and $taskAlphaManifest.amdTuning.installedInGame -and $taskAlphaManifest.amdTuning.identityAndGeometryCheckedAtRuntime) 'optional tuning metadata does not describe the managed artifact'
  Assert-Nr ((Get-NrHash (Get-NrChild $taskParams.OutputDirectory $taskAlphaManifest.amdTuning.file)) -eq $taskTuningHash -and $taskAlphaManifest.amdArithmeticDefault -eq 'k16' -and $taskAlphaManifest.nrEnabledByDefault -eq $false) 'optional tuning changed defaults or copied different bytes'
  Assert-Nr ($taskAlphaManifest.releaseChannel -eq 'alpha' -and $taskAlphaManifest.releaseTag -eq 'v0.1.0-alpha.99' -and $taskAlphaManifest.sourceCommit -ceq ('a' * 40)) 'versioned alpha metadata missing or source commit not normalized'
  $taskGuideHash = Get-NrHash (Join-Path $taskRoot 'docs\INSTALL.md')
  Assert-Nr ($taskAlphaManifest.installationGuide.file -eq 'INSTALL.md' -and $taskAlphaManifest.installationGuide.sha256 -eq $taskGuideHash -and (Get-NrHash (Join-Path $taskParams.OutputDirectory 'INSTALL.md')) -eq $taskGuideHash -and (Get-NrHash (Join-Path $taskParams.OutputDirectory 'source\OpenDLSS-NR-AMD\docs\INSTALL.md')) -eq $taskGuideHash) 'release install guide is missing, unhashed or differs from corresponding source'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskParams.OutputDirectory 'PACKAGE-README.txt') -Raw).Contains('Source commit: ' + ('a' * 40))) 'versioned package instructions omit the source commit'
  $null = $taskParams.Remove('ReleaseTag')
  $null = $taskParams.Remove('SourceCommit')
  $taskParams.OutputDirectory = Join-Path $taskScratch 'duplicate-test'
  Copy-Item -LiteralPath (Join-Path $taskKernels 'kernel.spv') -Destination (Join-Path $taskGameShaders 'kernel.spv')
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'duplicate shader filename accepted'
  Assert-Nr (-not (Test-Path -LiteralPath $taskParams.OutputDirectory)) 'failed package published output'
  $taskParams.ShaderFileNames = @('../kernel.spv')
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\package.ps1') @taskParams } 'shader selection traversal accepted'
  Write-Host "Package: $taskChecks synthetic checks passed; no game directory was used."
} finally {
  $taskResolved = [IO.Path]::GetFullPath($taskScratch)
  $taskExpected = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')).TrimEnd('\') + '\package-tests-'
  if (-not $taskResolved.StartsWith($taskExpected, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing unsafe test cleanup path' }
  # Remove regular files, then deepest directories. This avoids a PowerShell
  # recursive-delete race on large freshly copied source trees.
  Get-ChildItem -LiteralPath $taskResolved -File -Recurse -Force | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
  Get-ChildItem -LiteralPath $taskResolved -Directory -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
  Remove-Item -LiteralPath $taskResolved -Force
}
