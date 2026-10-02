param(
  [string]$RuntimeDll = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\game\OpenNrRuntime.dll'),
  [string]$DiagnosticExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dlss5vk.exe'),
  [Parameter(Mandatory = $true)][string]$OptiScalerDll,
  [string]$ShaderDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\shaders'),
  [string[]]$ShaderFileNames,
  [string]$GameShaderDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\game\shaders'),
  [string]$BridgeShader = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\game\shaders\bridge.hlsl'),
  [Parameter(Mandatory = $true)][string]$OutputDirectory,
  [string]$OptiScalerSource = (Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\optiscaler-host'),
  [string]$FidelityFxDirectory,
  [string]$AgilityDirectory,
  [string]$Configuration,
  [string]$ReleaseTag,
  [string]$SourceCommit
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'install_common.ps1')
$taskRoot = Get-NrRoot (Split-Path -Parent $PSScriptRoot)
$taskInstallGuide = Join-Path $taskRoot 'docs\INSTALL.md'
if ($ReleaseTag -and $ReleaseTag -cnotmatch '\Av[0-9]+\.[0-9]+\.[0-9]+-alpha\.[0-9]+\z') { throw 'ReleaseTag must identify an alpha version, for example v0.1.0-alpha.1' }
if ($SourceCommit -and $SourceCommit -notmatch '\A[a-fA-F0-9]{40}\z') { throw 'SourceCommit must be a full 40-character Git commit hash' }
if ($ReleaseTag -and -not $SourceCommit) { throw 'A versioned alpha package requires SourceCommit' }
if ($ReleaseTag -and -not (Test-Path -LiteralPath $taskInstallGuide -PathType Leaf)) { throw 'A versioned alpha package requires docs/INSTALL.md' }
if ($SourceCommit) { $SourceCommit = $SourceCommit.ToLowerInvariant() }
$taskStaticSources = @((Get-Content -LiteralPath (Join-Path $taskRoot 'integrations\optiscaler\sources\manifest.json') -Raw | ConvertFrom-Json).archives)
foreach ($taskDependency in $taskStaticSources) {
  if ($taskDependency.file -notmatch '^[a-z0-9.-]+\.tar\.(?:gz|xz)$') { throw 'Invalid static dependency source archive name' }
  $taskArchive = Join-Path $taskRoot ('integrations\optiscaler\sources\' + $taskDependency.file)
  if ((Get-Item -LiteralPath $taskArchive).Length -ne $taskDependency.length -or (Get-NrHash $taskArchive) -ne $taskDependency.sha256) { throw "Pinned $($taskDependency.name) source archive length/hash mismatch" }
}
$taskShaders = Get-NrRoot $ShaderDirectory
$taskHostSource = Get-NrRoot $OptiScalerSource
if (-not $FidelityFxDirectory) { $FidelityFxDirectory = Join-Path $taskHostSource 'external\FidelityFX-SDK-v2\Kits\FidelityFX\signedbin' }
if (-not $AgilityDirectory) { $AgilityDirectory = Join-Path $taskHostSource 'external\directx_agility_sdk\lib' }
$taskFfxRoot = Get-NrRoot $FidelityFxDirectory
$taskAgilityRoot = Get-NrRoot $AgilityDirectory
$taskDependencyNotices = @(
  @((Join-Path $taskHostSource 'external\FidelityFX-SDK-v2\docs\license.md'), 'OptiScaler\licenses\FidelityFX-license.md'),
  @((Join-Path $taskHostSource 'external\FidelityFX-SDK-v2\3rdpartynotice.md'), 'OptiScaler\licenses\FidelityFX-third-party-notices.md'),
  @((Join-Path $taskHostSource 'external\directx_agility_sdk\LICENSE.txt'), 'OptiScaler\licenses\Microsoft-DirectX-license.txt')
)
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
if (Test-Path -LiteralPath $taskOutput) { throw 'Package destination already exists; choose a new directory.' }
if ($taskOutput.Equals($taskRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $taskRoot.StartsWith($taskOutput + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid package destination' }
foreach ($taskInput in @($RuntimeDll, $OptiScalerDll, $DiagnosticExe, $BridgeShader, (Join-Path $taskRoot 'build\importer\model_importer.exe'))) {
  $null = Get-NrHash ([IO.Path]::GetFullPath($taskInput))
}
foreach ($taskName in @('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll')) { $null = Get-NrHash (Join-Path $taskFfxRoot $taskName) }
$taskAgilityDlls = @(Get-ChildItem -LiteralPath $taskAgilityRoot -File -Filter '*.dll')
if ($taskAgilityDlls.Name -notcontains 'D3D12Core.dll') { throw 'DirectX Agility D3D12Core.dll is required' }
foreach ($taskNotice in $taskDependencyNotices) { $null = Get-NrHash $taskNotice[0] }
$taskStageName = '.open-nr-package-' + [Guid]::NewGuid().ToString('N')
$taskStage = Join-Path (Split-Path -Parent $taskOutput) $taskStageName
if (Test-Path -LiteralPath $taskStage) { throw 'Cannot reserve package staging directory' }
New-Item -ItemType Directory -Path $taskStage -Force | Out-Null
$taskFiles = [Collections.Generic.List[object]]::new()
function Add-NrPayload([string]$Source, [string]$Destination, [string]$Role) {
  $taskTarget = Get-NrChild $taskStage ('payload\' + $Destination)
  New-Item -ItemType Directory -Path (Split-Path -Parent $taskTarget) -Force | Out-Null
  Copy-Item -LiteralPath $Source -Destination $taskTarget
  $taskPayloadInfo = [ordered]@{
    source = 'payload/' + $Destination.Replace('\', '/'); destination = $Destination.Replace('\', '/');
    role = $Role; sha256 = Get-NrHash $taskTarget
  }
  if ([IO.Path]::GetExtension($taskTarget) -eq '.dll') {
    $taskVersion = (Get-Item -LiteralPath $taskTarget).VersionInfo
    $taskPayloadInfo.fileVersion = $taskVersion.FileVersion
    $taskPayloadInfo.productVersion = $taskVersion.ProductVersion
  }
  $taskFiles.Add([pscustomobject]$taskPayloadInfo)
}
Add-NrPayload ([IO.Path]::GetFullPath($RuntimeDll)) 'OpenNrRuntime.dll' 'runtime'
Add-NrPayload ([IO.Path]::GetFullPath($OptiScalerDll)) 'OptiScaler.dll' 'proxy'
$taskSpvs = @(Get-ChildItem -LiteralPath $taskShaders -File -Filter '*.spv')
if ($ShaderFileNames) {
  $taskRequested = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($taskName in $ShaderFileNames) {
    if ([IO.Path]::GetFileName($taskName) -ne $taskName -or [IO.Path]::GetExtension($taskName) -ne '.spv' -or -not $taskRequested.Add($taskName)) { throw 'Shader selection requires unique SPIR-V basenames' }
    if ($taskSpvs.Name -notcontains $taskName) { throw "Selected shader is missing: $taskName" }
  }
  $taskSpvs = @($taskSpvs | Where-Object { $taskRequested.Contains($_.Name) })
}
if ($taskSpvs.Count -eq 0) { throw 'No compiled SPIR-V shaders were supplied' }
foreach ($taskShader in $taskSpvs) { Add-NrPayload $taskShader.FullName ('open-nr\shaders\' + $taskShader.Name) 'shader' }
if (Test-Path -LiteralPath $GameShaderDirectory -PathType Container) {
  $taskGameShaderRoot = Get-NrRoot $GameShaderDirectory
  foreach ($taskShader in (Get-ChildItem -LiteralPath $taskGameShaderRoot -File -Filter '*.spv')) {
    if ($taskSpvs.Name -contains $taskShader.Name) { throw "Duplicate kernel filename: $($taskShader.Name)" }
    Add-NrPayload $taskShader.FullName ('open-nr\shaders\' + $taskShader.Name) 'shader'
  }
}
Add-NrPayload ([IO.Path]::GetFullPath($BridgeShader)) 'open-nr\shaders\bridge.hlsl' 'shader-source'
foreach ($taskName in @('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll')) {
  Add-NrPayload (Join-Path $taskFfxRoot $taskName) ('OptiScaler\' + $taskName) 'dependency'
}
foreach ($taskDll in $taskAgilityDlls) { Add-NrPayload $taskDll.FullName ('OptiScaler\D3D12_OptiScaler\' + $taskDll.Name) 'dependency' }
foreach ($taskNotice in $taskDependencyNotices) { Add-NrPayload $taskNotice[0] $taskNotice[1] 'dependency-notice' }
if ($Configuration) {
  Add-NrPayload ([IO.Path]::GetFullPath($Configuration)) 'OptiScaler.ini' 'configuration'
} else {
  $taskIni = Join-Path $taskStage 'OptiScaler-default.ini'
  @'
[Upscalers]
Dx12Upscaler=ffx

[DlssNr]
Enabled=false
NrBackend=mochizuki
ApplyAfterRR=false
MochizukiPasses=1
MochizukiModelScale=1
MochizukiTemporal=true
MochizukiHistoryStrength=1
MochizukiDetailStrength=1
MochizukiColourStrength=1
MochizukiMaxRatio=4
MochizukiDynamicResolution=exact
MochizukiStabilizerStrength=0
'@ | Set-Content -LiteralPath $taskIni -Encoding ascii
  Add-NrPayload $taskIni 'OptiScaler.ini' 'configuration'
  Remove-Item -LiteralPath $taskIni
}
New-Item -ItemType Directory -Path (Join-Path $taskStage 'scripts'), (Join-Path $taskStage 'tools'), (Join-Path $taskStage 'source') | Out-Null
foreach ($taskScript in @('install.ps1', 'uninstall.ps1', 'install_common.ps1', 'import_model.ps1', 'fetch_presentmon.ps1')) {
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskScript) -Destination (Join-Path $taskStage 'scripts')
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'build\importer\model_importer.exe') -Destination (Join-Path $taskStage 'tools')
Copy-Item -LiteralPath ([IO.Path]::GetFullPath($DiagnosticExe)) -Destination (Join-Path $taskStage 'tools\dlss5vk.exe')
foreach ($taskTool in @('analyze_performance.py', 'compare_images.py', 'analyze_runtime.py', 'analyze_presentmon.py')) {
  Copy-Item -LiteralPath (Join-Path $taskRoot ('tools\' + $taskTool)) -Destination (Join-Path $taskStage 'tools')
}
foreach ($taskNotice in @('LICENSE', 'NOTICE', 'tools\MODEL_IMPORTER_NOTICE.txt', 'docs\AMD.md', 'docs\amd-numerics.md', 'docs\rx9070xt-validation.md')) {
  Copy-Item -LiteralPath (Join-Path $taskRoot $taskNotice) -Destination $taskStage
}
if (Test-Path -LiteralPath $taskInstallGuide -PathType Leaf) { Copy-Item -LiteralPath $taskInstallGuide -Destination (Join-Path $taskStage 'INSTALL.md') }
foreach ($taskNotice in @('FreeType-FTL.TXT', 'FreeType-LICENSE.TXT', 'Detours-LICENSE.md', 'FSR2-DX11-LICENSE.txt', 'FSR2-212-LICENSE.txt', 'FSR3-DX11-LICENSE.txt')) {
  Copy-Item -LiteralPath (Join-Path $taskRoot ('integrations\optiscaler\notices\' + $taskNotice)) -Destination $taskStage
}
# Corresponding source is shipped with the package, including the modified GPL
# host and build/install scripts. Only allowlisted source formats are copied;
# proprietary DLLs, generated model files, and local fixture captures cannot enter.
$taskExtensions = @('.cpp', '.c', '.cc', '.cxx', '.h', '.hpp', '.inl', '.inc', '.patch', '.comp', '.glsl', '.hlsl', '.hlsli', '.fx', '.frag', '.vert', '.spv', '.wgsl', '.html', '.css', '.js', '.mjs', '.ts', '.svg', '.json', '.py', '.ps1', '.cmd', '.bat', '.sh', '.md', '.txt', '.def', '.rc', '.rc2', '.ico', '.sln', '.vcxproj', '.filters', '.props', '.targets', '.yaml', '.yml', '.cmake', '.in', '.manifest', '.natvis', '.lib')
function Copy-NrSources([string]$SourceRoot, [string]$Prefix) {
  $taskDestination = Get-NrChild $taskStage ('source\' + $Prefix)
  $null = [IO.Directory]::CreateDirectory($taskDestination)
  $taskPrefix = $taskDestination.TrimEnd('\') + '\'
  $taskPending = [Collections.Generic.Stack[string]]::new()
  $taskPending.Push($SourceRoot)
  $taskMadeDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  # Prune generated directories before descending. Avoid scanning Git object
  # stores and keep source-tree copying practical on Windows.
  while ($taskPending.Count -gt 0) {
    foreach ($taskItem in (Get-ChildItem -LiteralPath $taskPending.Pop() -Force)) {
      if ($taskItem.PSIsContainer -and $taskItem.Name -in @('.git','build','__pycache__','node_modules','obj','fixtures','captures','models','model','dist','.cache')) { continue }
      if ($taskItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source package contains a reparse point' }
      if ($taskItem.PSIsContainer) { $taskPending.Push($taskItem.FullName); continue }
      $taskRelative = $taskItem.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
      if ($taskRelative -match '(?i)(weights\.bin|stage\d+\.bin|dlssnr\.bin|model-files\.sha256)') { continue }
      if ($taskItem.Extension.ToLowerInvariant() -notin $taskExtensions -and
          $taskItem.Name -notmatch '^(?i:LICENSE|COPYING|NOTICE|AUTHORS|COPYRIGHT|PATENTS)(?:[._-].*)?$' -and
          $taskItem.Name -notin @('CMakeLists.txt', '.gitmodules', '.gitignore', '.gitattributes')) { continue }
      # Source paths come from regular files, not a manifest. The staging tree
      # was freshly reserved. Verify containment and copy through literal .NET
      # paths, preserving tracked SDK linker libraries and their notices.
      $taskTarget = [IO.Path]::GetFullPath((Join-Path $taskDestination $taskRelative))
      if (-not $taskTarget.StartsWith($taskPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Source copy escapes staging tree' }
      $taskParent = [IO.Path]::GetDirectoryName($taskTarget)
      if ($taskMadeDirectories.Add($taskParent)) { $null = [IO.Directory]::CreateDirectory($taskParent) }
      [IO.File]::Copy($taskItem.FullName, $taskTarget, $false)
    }
  }
}
foreach ($taskDirectory in @('src', 'shaders', 'game', 'integrations', 'scripts', 'tests', 'docs', 'tools')) {
  $taskSource = Join-Path $taskRoot $taskDirectory
  if (Test-Path -LiteralPath $taskSource -PathType Container) { Copy-NrSources $taskSource ('OpenDLSS-NR-AMD\' + $taskDirectory) }
}
$taskBrowserSource = Join-Path $taskRoot 'ports\browser-webgpu'
if (Test-Path -LiteralPath $taskBrowserSource -PathType Container) { Copy-NrSources $taskBrowserSource 'OpenDLSS-NR-AMD\ports\browser-webgpu' }
foreach ($taskFile in @('LICENSE', 'NOTICE', 'README.md', '.gitignore')) {
  Copy-Item -LiteralPath (Join-Path $taskRoot $taskFile) -Destination (Join-Path $taskStage 'source\OpenDLSS-NR-AMD')
}
# Preserve complete official static-dependency source archives, including build
# files and notices outside the application's source-extension allowlist.
foreach ($taskDependency in $taskStaticSources) {
  $taskArchive = Join-Path $taskRoot ('integrations\optiscaler\sources\' + $taskDependency.file)
  $taskArchiveTarget = Get-NrChild $taskStage ('source\OpenDLSS-NR-AMD\integrations\optiscaler\sources\' + $taskDependency.file)
  $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($taskArchiveTarget))
  [IO.File]::Copy($taskArchive, $taskArchiveTarget, $false)
}
Copy-NrSources $taskHostSource 'OpenDLSS-NR-AMD\third_party\optiscaler-host'
$taskHostDistributionNotices = Join-Path $taskHostSource 'dist\streamline'
if (Test-Path -LiteralPath $taskHostDistributionNotices -PathType Container) {
  # Preserve the pinned host's text notices without redistributing its NVIDIA
  # DLLs. Normal source traversal deliberately prunes binary distribution trees.
  Copy-NrSources $taskHostDistributionNotices 'OpenDLSS-NR-AMD\third_party\optiscaler-host\dist\streamline'
}
$taskReadme = @'
OpenNR-AMD development validation package

This build has unmet performance and matched-image quality release gates.
Three Cyberpunk 2077 runs reported 4.56 / 4.56 / 4.57 built-in benchmark FPS
at 1440p display output with in-game frame generation off. Driver AFMF remains
unverified, so real-rendered FPS is not asserted. Completed NR+bridge jobs
took approximately 213 ms. The 60 FPS target has not been reached. See
rx9070xt-validation.md for measured settings, scope, runtime hashes and
remaining temporal/image quality checks. Treat this as a development build.

Start with INSTALL.md for prerequisites, installation, local model import,
enabling NR and reversible removal. The generated default OptiScaler.ini has
NR disabled (Enabled=false). To opt in after importing the model, open Insert
-> Neural -> Enable NR. Setting effect strengths to zero still runs inference.
Expect approximately 4.6 built-in benchmark FPS while NR is enabled on the
tested RX 9070 XT configuration. A supplied custom INI can override this default.

The OptiScaler host and its derived integration code are GPL-3.0. Their
corresponding source and build scripts are included under source/.
The original neural core and the model importer retain their MIT notices.
The AMD FidelityFX and Microsoft DirectX dependencies retain their separate
binary distribution terms and included notices under payload/OptiScaler/licenses/.
The OptiScaler binary is based in part on the work of the FreeType Team
(https://www.freetype.org). FreeType license texts are included alongside this
document and their provenance is recorded in the corresponding source.
Official FreeType 2.13.3, Microsoft Detours 4.0.1, and version-matched custom
FSR sources accompany the supplied static linker inputs. FSR31 local symbol
renames are documented with a checked source patch. The current host build
links the pinned host's supplied libraries; their original configuration and
byte reproduction have not been established. See
source/OpenDLSS-NR-AMD/integrations/optiscaler/sources/.
NVIDIA binaries and model weights are not included. Import a locally supplied
strictly pinned nvngx_dlssnr.dll 310.8.0 or 310.8.SF.0 with tools/model_importer.exe into the game's
open-nr/model directory. See AMD.md for setup and validation limitations.

From PowerShell:
  scripts/install.ps1 -PackageDirectory . -GameDirectory <game executable folder> -WhatIf
  scripts/install.ps1 -PackageDirectory . -GameDirectory <game executable folder>
  tools/model_importer.exe <local nvngx_dlssnr.dll> <game executable folder>/open-nr/model
  scripts/uninstall.ps1 -GameDirectory <game executable folder>

Uninstall restores verified originals and leaves separately imported models.
Changed managed files block uninstall until preserved or restored by the user.
'@
if ($ReleaseTag) { $taskReadme = "OpenNR-AMD $ReleaseTag alpha package`r`nSource commit: $SourceCommit`r`nThis commit identifies the packaged application source snapshot.`r`nPayload binary hashes are recorded separately in package-manifest.json.`r`n`r`n" + $taskReadme }
$taskReadme | Set-Content -LiteralPath (Join-Path $taskStage 'PACKAGE-README.txt') -Encoding utf8
$taskManifest = [pscustomobject]@{
  format = 'OpenNR-AMD-package-v1'; createdUtc = [DateTime]::UtcNow.ToString('o');
  releaseChannel = if ($ReleaseTag) { 'alpha' } else { 'development' };
  releaseTag = if ($ReleaseTag) { $ReleaseTag } else { $null };
  sourceCommit = if ($SourceCommit) { $SourceCommit } else { $null };
  nrEnabledByDefault = if ($Configuration) { $null } else { $false };
  installationGuide = if (Test-Path -LiteralPath (Join-Path $taskStage 'INSTALL.md') -PathType Leaf) { [ordered]@{ file = 'INSTALL.md'; sha256 = Get-NrHash (Join-Path $taskStage 'INSTALL.md') } } else { $null };
  adapterSourcePin = 'MatheusFerreiraS/neural-amd-opti@557bb8553098395f5f138c2e22ed25f256f7a3a2';
  importerSourcePin = 'mochizuki0323/DLSSNR-AMD@82560c4fbfaac347fc5e22c22025191402ae916b';
  sourceDependencies = @($taskStaticSources | ForEach-Object {
    [ordered]@{ name = $_.name; version = $_.version; source = 'source/OpenDLSS-NR-AMD/integrations/optiscaler/sources/' + $_.file; length = $_.length; sha256 = $_.sha256; upstreamCommit = $_.upstreamCommit; hostBuildInput = $_.hostBuildInput; localSourcePatch = if ($_.PSObject.Properties['localSourcePatch']) { $_.localSourcePatch } else { $null }; byteReproductionEstablished = $false }
  });
  diagnosticTools = @(
    [ordered]@{ file = 'tools/dlss5vk.exe'; sha256 = Get-NrHash (Join-Path $taskStage 'tools\dlss5vk.exe'); installedInGame = $false; purpose = 'Model, operator and recorded-frame replay diagnostics; not an end-to-end game benchmark' },
    [ordered]@{ file = 'tools/model_importer.exe'; sha256 = Get-NrHash (Join-Path $taskStage 'tools\model_importer.exe'); installedInGame = $false; purpose = 'Static bounded model inspection/import with strict DLL/resource hashes' }
  );
  validationStatus = 'development; weights excluded; game quality/performance release gate not established';
  releaseGates = [ordered]@{
    exactWholeModelReference = [ordered]@{ status = 'unmet'; requirement = 'Independent matched whole-model reference including temporal history; synthetic operator tests alone do not satisfy this gate' };
    optimizedComposedRgb = [ordered]@{ status = 'unmet'; minimumPsnrDb = 40; minimumSsim = 0.99; requirement = 'Every matched composed RGB frame, with matching model/input/control/history metadata' };
    gameTemporalHdr = [ordered]@{ status = 'unmet'; displayScope = 'initial SDR'; hdrDisplayValidation = 'deferred by user; outside the initial SDR release gate'; requirement = 'Cyberpunk motion, cuts, exposure, highlights, depth disocclusion, resize and reversible color-state validation for the initial SDR display target' };
    game1440p60fps = [ordered]@{ status = 'unmet'; outputWidth = 2560; outputHeight = 1440; minimumRenderedFps = 60; requirement = 'Real game measurement with application/driver frame generation off, no NR bypasses, measured GPU memory and repeated runs' }
  };
  target = 'Windows x64 / Radeon RX 9070 XT'; files = @($taskFiles.ToArray())
}
$taskManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskStage 'package-manifest.json') -Encoding utf8
$null = Read-NrPackage $taskStage
if (Test-Path -LiteralPath $taskOutput) { throw 'Package destination appeared while staging' }
Move-Item -LiteralPath $taskStage -Destination $taskOutput
Write-Host "Package written to $taskOutput (no NVIDIA DLL or model weights)."
