param(
  [switch]$Run, [switch]$SkipCore,
  [string]$OutputDirectory,
  [string]$RuntimeDll,
  [string]$RuntimeAssets,
  [ValidateRange(192,3840)][int]$Width = 320,
  [ValidateRange(128,2160)][int]$Height = 320,
  [ValidateRange(2,120)][int]$Frames = 2,
  [switch]$Trace, [switch]$Capture,
  [ValidateRange(1,120)][int]$CaptureFrames=1
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root 'build' }
$out = Join-Path $buildRoot 'interop'
New-Item -ItemType Directory -Force "$out\obj" | Out-Null
$vcvars = & "$PSScriptRoot\find_vcvars.ps1"
$core = @('src\vk_context.cpp','src\kernels.cpp','src\nr_graph.cpp','src\nr_model.cpp','tools\volk\volk.c')
$inputs = @('tests\interop_selftest.cpp')
if ($SkipCore) {
  foreach ($source in $core) {
    $object = Join-Path "$out\obj" (([IO.Path]::GetFileNameWithoutExtension($source)) + '.obj')
    if (-not (Test-Path -LiteralPath $object)) { throw "Missing $object; run without -SkipCore first" }
    $inputs += $object
  }
} else { $inputs += $core | ForEach-Object { Join-Path $root $_ } }
$sources = $inputs | ForEach-Object { if (-not [IO.Path]::IsPathRooted($_)) { '"' + (Join-Path $root $_) + '"' } else { '"' + $_ + '"' } }
$hostDefine = ''
if (Test-Path -LiteralPath "$root\third_party\optiscaler-host\OptiScaler\dlssnr\submission\CommandListProxy.h") { $hostDefine = '/DNR_HOST_BINDINGS_SELFTEST' }
$cmd = '"' + $vcvars + '" >nul && cl /nologo /std:c++20 /EHsc /O2 /W3 /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_USE_PLATFORM_WIN32_KHR /DVK_ENABLE_BETA_EXTENSIONS ' + $hostDefine + ' /I"' + $root + '\tools\Vulkan-Headers\include" /I"' + $root + '\tools\volk" /I"' + $root + '\src" /Fo"' + $out + '\obj\\" ' + ($sources -join ' ') + ' /Fe:"' + $out + '\interop_selftest.exe" /link d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib uuid.lib'
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'Interop selftest build failed' }
Write-Host "Built $out\interop_selftest.exe"
if ($Run) {
  $taskPreviousEnvironment = @{}
  try {
    $taskRuntimeDll = if ($RuntimeDll) { [IO.Path]::GetFullPath($RuntimeDll) } else { Join-Path $buildRoot 'game\OpenNrRuntime.dll' }
    if (-not (Test-Path -LiteralPath $taskRuntimeDll -PathType Leaf)) { throw "Missing runtime DLL: $taskRuntimeDll; build_game.ps1 must use the same OutputDirectory" }
    $taskPreviousEnvironment['OPEN_NR_RUNTIME_DLL'] = [Environment]::GetEnvironmentVariable('OPEN_NR_RUNTIME_DLL', 'Process')
    [Environment]::SetEnvironmentVariable('OPEN_NR_RUNTIME_DLL', $taskRuntimeDll, 'Process')
    if ($RuntimeAssets) {
      $taskAssets = (Resolve-Path -LiteralPath $RuntimeAssets).Path
      if (-not (Test-Path -LiteralPath (Join-Path $taskAssets 'model\manifest.json'))) { throw 'RuntimeAssets must contain model/manifest.json and shaders/.' }
      $taskSettings = @{
        OPEN_NR_RUNTIME_SELFTEST = '1'; OPEN_NR_RUNTIME_ASSETS = $taskAssets;
        OPEN_NR_RUNTIME_WIDTH = [string]$Width; OPEN_NR_RUNTIME_HEIGHT = [string]$Height;
        OPEN_NR_RUNTIME_FRAMES = [string]$Frames; OPEN_NR_RUNTIME_TRACE_SELFTEST = $(if ($Trace) { '1' } else { $null })
        OPEN_NR_RUNTIME_CAPTURE_SELFTEST = $(if ($Capture) { '1' } else { $null })
        OPEN_NR_RUNTIME_CAPTURE_FRAMES = $(if ($Capture) { [string]$CaptureFrames } else { $null })
      }
      foreach ($taskName in $taskSettings.Keys) {
        $taskPreviousEnvironment[$taskName] = [Environment]::GetEnvironmentVariable($taskName, 'Process')
        [Environment]::SetEnvironmentVariable($taskName, $taskSettings[$taskName], 'Process')
      }
    }
    & "$out\interop_selftest.exe" $root
    if ($LASTEXITCODE -ne 0) { throw 'Interop selftest failed' }
  } finally {
    foreach ($taskName in $taskPreviousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($taskName, $taskPreviousEnvironment[$taskName], 'Process') }
  }
}
