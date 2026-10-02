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
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\open-nr\shaders\experimental.spv'))) 'unselected experimental shader entered payload'
  Assert-Nr (@($taskManifest.releaseGates.PSObject.Properties | Where-Object { $_.Value.status -ne 'unmet' }).Count -eq 0 -and @($taskManifest.releaseGates.PSObject.Properties).Count -eq 4) 'unmet numerical/game release gates missing'
  Assert-Nr ($taskManifest.releaseGates.gameTemporalHdr.displayScope -eq 'initial SDR' -and $taskManifest.releaseGates.gameTemporalHdr.hdrDisplayValidation -like 'deferred*') 'HDR display deferral is missing from the initial game gate'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'amd-numerics.md')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'rx9070xt-validation.md'))) 'linked package-root validation documents are missing'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'FreeType-FTL.TXT')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'FreeType-LICENSE.TXT'))) 'FreeType binary notices are missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\open-nr\shaders\bridge.hlsl')) 'bridge HLSL missing'
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskOutput 'payload\open-nr\shaders\game_preprocess.spv')) 'game shader missing'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'tools\analyze_runtime.py')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'tools\analyze_presentmon.py')) -and (Test-Path -LiteralPath (Join-Path $taskOutput 'scripts\fetch_presentmon.ps1'))) 'diagnostic tools are missing from package'
  Assert-Nr ((Test-Path -LiteralPath (Join-Path $taskOutput 'tools\dlss5vk.exe')) -and @($taskManifest.diagnosticTools).Count -eq 2 -and @($taskManifest.files | Where-Object { $_.destination -like '*.exe' }).Count -eq 0) 'diagnostic executable missing, unhashed or scheduled for game installation'
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
