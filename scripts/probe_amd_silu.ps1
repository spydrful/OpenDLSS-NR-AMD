# Isolated preserving SiLU diagnostic. Default builds and runs CPU cases only.
param(
  [string]$OutputDirectory = ('build/performance/silu-rte-probe-' + [Guid]::NewGuid().ToString('N')),
  [switch]$RunGpu,
  [switch]$QueryDevice
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOut = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
  [IO.Path]::GetFullPath($OutputDirectory)
} else { [IO.Path]::GetFullPath((Join-Path $taskRoot $OutputDirectory)) }
$taskPrivateRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')).TrimEnd('\', '/') + '\'
if (-not $taskOut.StartsWith($taskPrivateRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Diagnostic output must stay in the private build directory' }
if (Test-Path -LiteralPath $taskOut) { throw 'Diagnostic destination already exists; choose a new directory' }
if ($RunGpu -and $QueryDevice) { throw 'Select either RunGpu or QueryDevice' }
$taskSources = @('shaders/common.glsl','shaders/amd_half_publication_rte.glsl','shaders/amd_silu_rte.glsl','shaders/diagnostic/amd_silu_rte.comp','tests/amd_silu_rte_probe.cpp','src/numeric.h','src/sha256.h','src/vk_context.cpp','src/vk_context.h','src/amd_config.h','scripts/probe_amd_silu.ps1','scripts/complete_amd_silu_float_controls.ps1')
$taskSourceHashes = @{}
foreach ($taskSource in $taskSources) { $taskSourceHashes[$taskSource] = (Get-FileHash -LiteralPath (Join-Path $taskRoot $taskSource) -Algorithm SHA256).Hash.ToLowerInvariant() }
New-Item -ItemType Directory -Path $taskOut,(Join-Path $taskOut 'obj') | Out-Null
$taskShader = Join-Path $taskOut 'amd_silu_rte.spv'
$taskGlslang = Join-Path $taskRoot 'tools/glslang/bin/glslang.exe'
& $taskGlslang -V --target-env vulkan1.3 "-I$taskRoot/shaders" (Join-Path $taskRoot 'shaders/diagnostic/amd_silu_rte.comp') -o $taskShader
if ($LASTEXITCODE -ne 0) { throw 'SiLU diagnostic shader compilation failed' }
$taskModeClosure = & (Join-Path $PSScriptRoot 'complete_amd_silu_float_controls.ps1') -InputPath $taskShader -OutputPath $taskShader
$taskVcvars = & (Join-Path $PSScriptRoot 'find_vcvars.ps1')
$taskCommand = "`"$taskVcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /fp:strict /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS /I`"$taskRoot/tools/Vulkan-Headers/include`" /I`"$taskRoot/tools/volk`" /I`"$taskRoot/src`" /Fo`"$taskOut/obj/`" `"$taskRoot/tests/amd_silu_rte_probe.cpp`" `"$taskRoot/src/vk_context.cpp`" `"$taskRoot/tools/volk/volk.c`" /Fe:`"$taskOut/amd_silu_rte_probe.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $taskCommand
if ($LASTEXITCODE -ne 0) { throw 'SiLU diagnostic harness compilation failed' }
foreach ($taskSource in $taskSources) {
  if ((Get-FileHash -LiteralPath (Join-Path $taskRoot $taskSource) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskSourceHashes[$taskSource]) { throw "Diagnostic source changed during build: $taskSource" }
}
$taskMode = if ($RunGpu) { 'run' } elseif ($QueryDevice) { 'info' } else { 'cpu' }
$taskExecutable = Join-Path $taskOut 'amd_silu_rte_probe.exe'
$taskBuildIdentity = [ordered]@{
  format = 'OpenNR-silu-rte-probe-build-v1'; createdUtc = [DateTime]::UtcNow.ToString('o')
  mode = $taskMode; gpuRequested = [bool]$RunGpu; deviceInquiryRequested = [bool]$QueryDevice
  executableSha256 = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
  shaderSha256 = (Get-FileHash -LiteralPath $taskShader -Algorithm SHA256).Hash.ToLowerInvariant()
  glslangSha256 = (Get-FileHash -LiteralPath $taskGlslang -Algorithm SHA256).Hash.ToLowerInvariant()
  cpuFloatPolicy = '/fp:strict; explicit std::fma; RNE checked'; sources = $taskSourceHashes
  modeClosure = $taskModeClosure
  productionQualified = $false; performanceClaim = $false
}
$taskBuildIdentity | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOut 'build-identity.json') -Encoding utf8
& $taskExecutable $taskMode $taskShader (Join-Path $taskOut "$taskMode.json")
if ($LASTEXITCODE -ne 0) { throw "SiLU diagnostic failed (exit $LASTEXITCODE)" }
