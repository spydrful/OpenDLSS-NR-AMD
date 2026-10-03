param(
  [string]$OutputDirectory = 'build/performance/publication-rte-probe',
  [switch]$RunGpu
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$taskOut = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
  [IO.Path]::GetFullPath($OutputDirectory)
} else {
  [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
}
New-Item -ItemType Directory -Force -Path $taskOut,(Join-Path $taskOut 'obj') | Out-Null
$taskShader = Join-Path $taskOut 'amd_half_publication_rte.spv'
& (Join-Path $root 'tools/glslang/bin/glslang.exe') -V --target-env vulkan1.3 "-I$root/shaders" (Join-Path $root 'shaders/diagnostic/amd_half_publication_rte.comp') -o $taskShader
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic shader compilation failed' }
$taskVcvars = & (Join-Path $PSScriptRoot 'find_vcvars.ps1')
$taskCommand = "`"$taskVcvars`" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS /I`"$root/tools/Vulkan-Headers/include`" /I`"$root/tools/volk`" /I`"$root/src`" /Fo`"$taskOut/obj/`" `"$root/tests/amd_publication_probe.cpp`" `"$root/src/vk_context.cpp`" `"$root/tools/volk/volk.c`" /Fe:`"$taskOut/amd_publication_probe.exe`" /link /SUBSYSTEM:CONSOLE"
cmd /c $taskCommand
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic harness compilation failed' }
$taskMode = if ($RunGpu) { 'run' } else { 'info' }
& (Join-Path $taskOut 'amd_publication_probe.exe') $taskMode $taskShader (Join-Path $taskOut "$taskMode.json")
if ($LASTEXITCODE -ne 0) { throw "Diagnostic inquiry/probe did not qualify (exit $LASTEXITCODE)" }
