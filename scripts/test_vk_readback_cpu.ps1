# Compile and run the production readback range/dependency contract without Vulkan execution.
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root 'build\readback-cpu-tests' }
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = & (Join-Path $PSScriptRoot 'find_vcvars.ps1')
$source = Join-Path $root 'tests\vk_readback_tests.cpp'
$include = '/I"' + (Join-Path $root 'tools\volk') + '" /I"' + (Join-Path $root 'tools\Vulkan-Headers\include') + '"'
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\vk_readback_tests.obj`" `"$source`" /Fe:`"$out\vk_readback_tests.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'readback CPU contract compilation failed' }
& (Join-Path $out 'vk_readback_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'readback CPU contract failed' }
