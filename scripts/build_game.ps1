param([switch]$SkipCore, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root 'build' }
if (-not $SkipCore) { & "$PSScriptRoot\build.ps1" -Backend amd -OutputDirectory $buildRoot; if ($LASTEXITCODE -ne 0) { throw 'Core build failed' } }
$out = Join-Path $buildRoot 'game'
New-Item -ItemType Directory -Force "$out\obj","$out\shaders" | Out-Null
$compiler = Join-Path $root 'tools\glslang\bin\glslang.exe'
foreach ($source in Get-ChildItem "$root\game\shaders\*.comp") {
  & $compiler -V --target-env vulkan1.3 "-I$root\game\shaders" $source.FullName -o "$out\shaders\$($source.BaseName).spv"
  if ($LASTEXITCODE -ne 0) { throw "Shader compilation failed: $($source.Name)" }
}
Copy-Item -LiteralPath "$root\game\shaders\bridge.hlsl" -Destination "$out\shaders\bridge.hlsl"
$vcvars = & "$PSScriptRoot\find_vcvars.ps1"
$sources = @('game\runtime.cpp','src\vk_context.cpp','src\kernels.cpp','src\nr_graph.cpp','src\nr_model.cpp','tools\volk\volk.c') | ForEach-Object { '"' + (Join-Path $root $_) + '"' }
$cmd = '"' + $vcvars + '" >nul && cl /nologo /std:c++20 /EHsc /O2 /W3 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DVK_USE_PLATFORM_WIN32_KHR /DVK_ENABLE_BETA_EXTENSIONS /I"' + $root + '\tools\Vulkan-Headers\include" /I"' + $root + '\tools\volk" /I"' + $root + '\src" /Fo"' + $out + '\obj\\" ' + ($sources -join ' ') + ' /link /DLL /OUT:"' + $out + '\OpenNrRuntime.dll" d3d12.lib d3dcompiler.lib dxgi.lib'
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'Native game runtime build failed' }
Write-Host "Built $out\OpenNrRuntime.dll"
