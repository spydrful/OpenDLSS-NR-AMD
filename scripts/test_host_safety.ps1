[CmdletBinding()]
param([switch]$RunGpu)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build\host-safety'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& "$PSScriptRoot\patch_optiscaler.ps1"
$vcvars = & "$PSScriptRoot\find_vcvars.ps1"
$command = '"' + $vcvars + '" >nul && cl /nologo /std:c++20 /EHsc /W4 /I"' + $root + '\third_party\optiscaler-host\OptiScaler\dlssnr\backend" "' + $root + '\tests\host_job_lifetime_tests.cpp" /Fe"' + $out + '\host_job_lifetime_tests.exe" /Fo"' + $out + '\host_job_lifetime_tests.obj" && "' + $out + '\host_job_lifetime_tests.exe"'
cmd /c $command
if ($LASTEXITCODE -ne 0) { throw 'Host lifetime tests failed' }
if ($RunGpu) {
    $command = '"' + $vcvars + '" >nul && call "' + $root + '\third_party\optiscaler-host\tools\test-lmxxf-evaluate-cut.cmd" "' + $out + '"'
    cmd /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Host evaluate-cut GPU tests failed' }
}
