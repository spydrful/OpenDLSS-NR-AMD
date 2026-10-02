param([switch]$Fetch)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$hostRoot = Join-Path $root 'third_party\optiscaler-host'
if ($Fetch -or -not (Test-Path -LiteralPath "$hostRoot\OptiScaler\OptiScaler.vcxproj")) {
  & "$PSScriptRoot\fetch_optiscaler.ps1"
}
& "$PSScriptRoot\patch_optiscaler.ps1"
# Deterministic version metadata replaces the upstream wall-clock pre-build event.
'#define VER_BUILD_DATE "OpenDLSS-NR-AMD"' | Set-Content -LiteralPath "$hostRoot\OptiScaler\resource_build_date.h" -Encoding ASCII
'#define VER_BUILD_COMMIT "557bb855+OpenNR"' | Set-Content -LiteralPath "$hostRoot\OptiScaler\resource_build_commit.h" -Encoding ASCII
$vcvars = & "$PSScriptRoot\find_vcvars.ps1"
$out = Join-Path $root 'build\optiscaler'
New-Item -ItemType Directory -Force $out | Out-Null
# Override upstream post-build packaging. Packaging is handled by package.ps1.
$targets = Join-Path $out 'NoPackaging.targets'
@'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup><PostBuildEvent><Command></Command></PostBuildEvent><PreBuildEvent><Command></Command></PreBuildEvent></ItemDefinitionGroup>
</Project>
'@ | Set-Content -LiteralPath $targets -Encoding UTF8
$command = '"' + $vcvars + '" >nul && msbuild "' + $hostRoot + '\OptiScaler\OptiScaler.vcxproj" /m /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="' + $hostRoot + '\\" /p:OutDir="' + $out + '\\" /p:ForceImportAfterCppTargets="' + $targets + '"'
$taskInfo = [Diagnostics.ProcessStartInfo]::new()
$taskInfo.FileName = $env:ComSpec
$taskInfo.Arguments = '/d /s /c "' + $command + '"'
$taskInfo.UseShellExecute = $false
$taskInfo.CreateNoWindow = $true
$taskInfo.RedirectStandardOutput = $true
$taskInfo.RedirectStandardError = $true
# Windows environment keys are case-insensitive. Desktop launchers can supply
# both PATH and Path, which MSBuild rejects before compiling. Normalize only
# this build child; do not change the user's environment or print its values.
$taskEnvironment = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($taskEntry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $taskEnvironment[$taskEntry.Key] = $taskEntry.Value }
$taskInfo.Environment.Clear()
foreach ($taskEntry in $taskEnvironment.GetEnumerator()) { $taskInfo.Environment[$taskEntry.Key.ToUpperInvariant()] = $taskEntry.Value }
$taskProcess = [Diagnostics.Process]::new()
$taskProcess.StartInfo = $taskInfo
if (-not $taskProcess.Start()) { throw 'Cannot launch pinned host build' }
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskBuildOutput = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $out 'last-build.log'), $taskBuildOutput)
Write-Host $taskBuildOutput
if ($taskProcess.ExitCode -ne 0) { throw "Pinned OptiScaler build failed; see $out\last-build.log" }
Write-Host "Built $out\OptiScaler.dll"
