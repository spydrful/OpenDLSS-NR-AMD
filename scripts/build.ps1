# Build shaders + dlss5vk.exe with the portable toolchain under tools/ and MSVC.
param(
  [switch]$Debug,
  [ValidateSet('amd','reference','nvidia','all')][string]$Backend = 'amd',
  [switch]$SkipShaders,
  [string]$OutputDirectory
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root 'build' }
if (-not $SkipShaders) {
  & (Join-Path $PSScriptRoot "build_shaders.ps1") -Backend $Backend -OutputDirectory $out
  if ($LASTEXITCODE -ne 0) { throw "shader compilation failed" }
}

$vcvars = & (Join-Path $PSScriptRoot "find_vcvars.ps1")
$sources = (Get-ChildItem (Join-Path $root "src\*.cpp") | ForEach-Object { '"' + $_.FullName + '"' }) -join " "
$testSource = Join-Path $root 'tests\native_selftest.cpp'
$testDefine = ''
if (Test-Path $testSource) { $sources += ' "' + $testSource + '"'; $testDefine = '/DNR_NATIVE_SELFTEST' }
$modelValidationSource = Join-Path $root 'tests\model_validation.cpp'
if (Test-Path $modelValidationSource) { $sources += ' "' + $modelValidationSource + '"'; $testDefine += ' /DNR_MODEL_VALIDATION' }
$compositeValidationSource = Join-Path $root 'tests\composite_validation.cpp'
if (Test-Path $compositeValidationSource) { $sources += ' "' + $compositeValidationSource + '"'; $testDefine += ' /DNR_COMPOSITE_VALIDATION' }
$amdPreservationSource = Join-Path $root 'tests\amd_kernel_preservation.cpp'
if (Test-Path $amdPreservationSource) { $sources += ' "' + $amdPreservationSource + '"'; $testDefine += ' /DNR_AMD_KERNEL_PRESERVATION' }
$volk = '"' + (Join-Path $root "tools\volk\volk.c") + '"'
$include = '/I"' + (Join-Path $root "tools\Vulkan-Headers\include") + '" /I"' + (Join-Path $root "tools\volk") + '" /I"' + (Join-Path $root "src") + '"'
$opt = if ($Debug) { "/Od /Zi" } else { "/O2" }
New-Item -ItemType Directory -Force (Join-Path $out "obj") | Out-Null
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 $opt /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS $testDefine $include /Fo`"$out\obj\\`" $sources $volk /Fe:`"$out\dlss5vk.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compilation failed" }
Write-Host "built $out\dlss5vk.exe"
