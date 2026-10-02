param(
  [Parameter(Mandatory = $true)][string]$PackageDirectory,
  [string]$GlslangExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\glslang\bin\glslang.exe')
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts\install_common.ps1')
$taskPackage = Get-NrRoot $PackageDirectory
$null = Read-NrPackage $taskPackage
$taskSource = Get-NrRoot (Join-Path $taskPackage 'source\OpenDLSS-NR-AMD')
$null = Get-NrHash ([IO.Path]::GetFullPath($GlslangExe))
$taskScratch = Join-Path $taskRoot ('build\source-package-test-' + [Guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($taskScratch)
$taskPrefix = [IO.Path]::GetFullPath($taskScratch).TrimEnd('\') + '\'
$taskPending = [Collections.Generic.Stack[string]]::new()
$taskPending.Push($taskSource)
$taskMadeDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($taskPending.Count -gt 0) {
  foreach ($taskItem in (Get-ChildItem -LiteralPath $taskPending.Pop() -Force)) {
    if ($taskItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source package contains a reparse point' }
    if ($taskItem.PSIsContainer) { $taskPending.Push($taskItem.FullName); continue }
    $taskRelative = $taskItem.FullName.Substring($taskSource.Length).TrimStart('\', '/')
    $taskTarget = [IO.Path]::GetFullPath((Join-Path $taskScratch $taskRelative))
    if (-not $taskTarget.StartsWith($taskPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe source test copy path' }
    $taskParent = [IO.Path]::GetDirectoryName($taskTarget)
    if ($taskMadeDirectories.Add($taskParent)) { $null = [IO.Directory]::CreateDirectory($taskParent) }
    [IO.File]::Copy($taskItem.FullName, $taskTarget, $false)
  }
}
# glslang is a build tool, not part of the linked GPL application. Use the same
# local cached compiler explicitly; its executable is not copied into releases.
$taskCompilerTarget = Join-Path $taskScratch 'tools\glslang\bin\glslang.exe'
$null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($taskCompilerTarget))
[IO.File]::Copy([IO.Path]::GetFullPath($GlslangExe), $taskCompilerTarget, $false)
Write-Host "Fresh source verification workspace: $taskScratch"
foreach ($taskSpec in @(
  @('build.ps1', 'native-core-build.log', @{ Backend = 'amd' }),
  @('build_game.ps1', 'runtime-build.log', @{ SkipCore = $true }),
  @('build_importer.ps1', 'importer-build.log', @{ Test = $true }),
  @('build_optiscaler.ps1', 'host-build.log', @{})
)) {
  $taskScript = Join-Path $taskScratch ('scripts\' + $taskSpec[0])
  $taskLog = Join-Path $taskScratch $taskSpec[1]
  $taskArguments = $taskSpec[2]
  Write-Host "Building $($taskSpec[0]); log $taskLog"
  $taskInfo = [Diagnostics.ProcessStartInfo]::new()
  $taskInfo.FileName = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
  $taskInfo.UseShellExecute = $false
  $taskInfo.CreateNoWindow = $true
  $taskInfo.RedirectStandardOutput = $true
  $taskInfo.RedirectStandardError = $true
  foreach ($taskArgument in @('-NoLogo','-NoProfile','-NonInteractive','-File',$taskScript)) { $taskInfo.ArgumentList.Add($taskArgument) }
  foreach ($taskArgument in $taskArguments.GetEnumerator()) {
    $taskInfo.ArgumentList.Add('-' + $taskArgument.Key)
    if ($taskArgument.Value -isnot [bool]) { $taskInfo.ArgumentList.Add([string]$taskArgument.Value) }
  }
  # Windows environment names are case-insensitive. A desktop runner can supply
  # both PATH and Path; old MSBuild rejects that duplicate before compiling.
  # Normalize names for this child only without printing environment values.
  $taskEnvironment = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($taskEntry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $taskEnvironment[$taskEntry.Key] = $taskEntry.Value }
  $taskInfo.Environment.Clear()
  foreach ($taskEntry in $taskEnvironment.GetEnumerator()) { $taskInfo.Environment[$taskEntry.Key.ToUpperInvariant()] = $taskEntry.Value }
  $taskProcess = [Diagnostics.Process]::new()
  $taskProcess.StartInfo = $taskInfo
  if (-not $taskProcess.Start()) { throw 'Cannot start source-package build child' }
  $taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
  $taskStderr = $taskProcess.StandardError.ReadToEndAsync()
  $taskProcess.WaitForExit()
  [IO.File]::WriteAllText($taskLog, $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult())
  if ($taskProcess.ExitCode -ne 0) { throw "Fresh source build returned $($taskProcess.ExitCode)`: $taskLog" }
}
foreach ($taskProduct in @('build\dlss5vk.exe','build\game\OpenNrRuntime.dll','build\importer\model_importer.exe','build\optiscaler\OptiScaler.dll')) {
  $null = Get-NrHash (Join-Path $taskScratch $taskProduct)
}
Write-Host "SOURCE PACKAGE BUILD PASS: native core/shaders, runtime, importer tests, and OptiScaler host rebuilt. Workspace retained at $taskScratch. No model or game was used."
