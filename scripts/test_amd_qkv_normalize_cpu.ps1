# Build and test QKV normalization policy/capabilities with no Vulkan execution.
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $root 'build\qkv-normalize-cpu-tests'}
New-Item -ItemType Directory -Force $out|Out-Null
$vcvars=& (Join-Path $PSScriptRoot 'find_vcvars.ps1')
$source=Join-Path $root 'tests\amd_qkv_normalize_tests.cpp'
$include='/I"'+(Join-Path $root 'tools\volk')+'" /I"'+(Join-Path $root 'tools\Vulkan-Headers\include')+'"'
$cmd="`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DVK_ENABLE_BETA_EXTENSIONS $include /Fo`"$out\amd_qkv_normalize_tests.obj`" `"$source`" /Fe:`"$out\amd_qkv_normalize_tests.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if($LASTEXITCODE -ne 0){throw 'QKV normalization CPU compilation failed'}
& (Join-Path $out 'amd_qkv_normalize_tests.exe') (Join-Path $root 'docs\performance\rx9070xt-26.9.1-rte-k16.json')
if($LASTEXITCODE -ne 0){throw 'QKV normalization CPU tests failed'}
