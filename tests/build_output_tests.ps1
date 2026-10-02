# Exercise actual build scripts with CPU-only compiler mocks in a fresh workspace.
# The production build directory and installed compilers are never invoked.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskScratch = Join-Path $taskRoot ('build\build-output-tests-' + [Guid]::NewGuid().ToString('N'))
$taskScripts = @('build.ps1', 'build_shaders.ps1', 'build_game.ps1', 'build_interop.ps1')
$taskChecks = 0
function Assert-TaskBuildOutput([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
  $script:taskChecks++
}
foreach ($taskName in $taskScripts) {
  $taskTokens = $null; $taskErrors = $null
  $taskAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $taskRoot "scripts\$taskName"), [ref]$taskTokens, [ref]$taskErrors)
  Assert-TaskBuildOutput ($taskErrors.Count -eq 0) "$taskName has PowerShell syntax errors"
  Assert-TaskBuildOutput (@($taskAst.ParamBlock.Parameters.Name.VariablePath.UserPath) -contains 'OutputDirectory') "$taskName lacks OutputDirectory"
}
$taskDirectories = @('scripts', 'src', 'shaders', 'game\shaders', 'tools\glslang\bin', 'docs\performance')
foreach ($taskDirectory in $taskDirectories) { $null = [IO.Directory]::CreateDirectory((Join-Path $taskScratch $taskDirectory)) }
foreach ($taskName in $taskScripts) { Copy-Item -LiteralPath (Join-Path $taskRoot "scripts\$taskName") -Destination (Join-Path $taskScratch "scripts\$taskName") }
[IO.File]::WriteAllText((Join-Path $taskScratch 'scripts\find_vcvars.ps1'), "'C:\mock-vcvars.bat'")
foreach ($taskSource in @('src\minimal.cpp', 'shaders\ops.comp', 'shaders\amd_gemm.comp', 'game\shaders\pack.comp', 'game\shaders\bridge.hlsl')) {
  [IO.File]::WriteAllText((Join-Path $taskScratch $taskSource), '// compiler is mocked')
}
[IO.File]::WriteAllText((Join-Path $taskScratch 'docs\performance\rx9070xt-26.9.1-k16.json'), '{"fixture":true}')
$taskPreferredTuning = Join-Path $taskScratch 'docs\performance\rx9070xt-26.9.1-direct-k16.json'
[IO.File]::WriteAllText($taskPreferredTuning, '{"fixture":"direct"}')
$taskFakeCompiler = Join-Path $taskScratch 'tools\glslang\bin\glslang.exe'
$taskFakeFunction = 'Function:\' + $taskFakeCompiler
$taskOldCmd = Get-Item -LiteralPath 'Function:\cmd' -ErrorAction SilentlyContinue
$global:NrBuildOutputShaderCalls = [Collections.Generic.List[object]]::new()
$global:NrBuildOutputCompilerCalls = [Collections.Generic.List[string]]::new()
$global:NrBuildOutputRuntimeCalls = [Collections.Generic.List[object]]::new()
$taskInteropExecutable = Join-Path $taskScratch 'interop experiment\interop\interop_selftest.exe'
$taskInteropFunction = 'Function:\' + $taskInteropExecutable
$taskRuntimeEnvironmentNames = @('OPEN_NR_RUNTIME_DLL', 'OPEN_NR_RUNTIME_SELFTEST', 'OPEN_NR_RUNTIME_ASSETS', 'OPEN_NR_RUNTIME_WIDTH', 'OPEN_NR_RUNTIME_HEIGHT', 'OPEN_NR_RUNTIME_FRAMES', 'OPEN_NR_RUNTIME_TRACE_SELFTEST', 'OPEN_NR_RUNTIME_CAPTURE_SELFTEST', 'OPEN_NR_RUNTIME_CAPTURE_FRAMES')
$taskOriginalRuntimeEnvironment = @{}
foreach ($taskName in $taskRuntimeEnvironmentNames) { $taskOriginalRuntimeEnvironment[$taskName] = [Environment]::GetEnvironmentVariable($taskName, 'Process') }
Set-Item -LiteralPath $taskFakeFunction -Value {
  $Arguments = @($args)
  $global:NrBuildOutputShaderCalls.Add(@($Arguments))
  $taskIndex = [Array]::IndexOf($Arguments, '-o')
  if ($taskIndex -lt 0) { throw 'Shader invocation has no output argument' }
  [IO.File]::WriteAllText([string]$Arguments[$taskIndex + 1], 'mock shader bytes')
  $global:LASTEXITCODE = 0
}
Set-Item -LiteralPath 'Function:\cmd' -Value {
  $Arguments = @($args)
  $global:NrBuildOutputCompilerCalls.Add(($Arguments -join ' '))
  $global:LASTEXITCODE = 0
}
Set-Item -LiteralPath $taskInteropFunction -Value {
  $global:NrBuildOutputRuntimeCalls.Add(@{repository=$args[0];dll=$env:OPEN_NR_RUNTIME_DLL;assets=$env:OPEN_NR_RUNTIME_ASSETS;enabled=$env:OPEN_NR_RUNTIME_SELFTEST;frames=$env:OPEN_NR_RUNTIME_FRAMES;trace=$env:OPEN_NR_RUNTIME_TRACE_SELFTEST;capture=$env:OPEN_NR_RUNTIME_CAPTURE_SELFTEST;capture_frames=$env:OPEN_NR_RUNTIME_CAPTURE_FRAMES})
  $global:LASTEXITCODE = 0
}
try {
  # Defaults still resolve below this copy's build/, never below the real repo.
  & (Join-Path $taskScratch 'scripts\build.ps1') -Backend amd
  $taskDefault = Join-Path $taskScratch 'build'
  Assert-TaskBuildOutput (Test-Path -LiteralPath "$taskDefault\shaders\ops.spv") 'Default shader path changed'
  Assert-TaskBuildOutput (Test-Path -LiteralPath "$taskDefault\shaders\amd-tuning.json") 'Default tuning copy missing'
  Assert-TaskBuildOutput (([IO.File]::ReadAllText("$taskDefault\shaders\amd-tuning.json")) -ceq '{"fixture":"direct"}') 'Build did not prefer the qualified direct tuning cache'
  Assert-TaskBuildOutput (([IO.File]::ReadAllText((Join-Path $taskScratch 'docs\performance\rx9070xt-26.9.1-k16.json'))) -ceq '{"fixture":true}') 'New tuning selection overwrote the legacy source cache'
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/Fe:`"$taskDefault\dlss5vk.exe`"")) 'Default executable path changed'
  $taskExplicit = Join-Path $taskScratch 'experiment output'
  & (Join-Path $taskScratch 'scripts\build.ps1') -Backend reference -OutputDirectory $taskExplicit
  Assert-TaskBuildOutput (Test-Path -LiteralPath "$taskExplicit\shaders\ops.spv") 'Core did not forward explicit shader destination'
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/Fe:`"$taskExplicit\dlss5vk.exe`"")) 'Explicit executable path ignored'
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/Fo`"$taskExplicit\obj\\`"")) 'Explicit object path ignored'
  Assert-TaskBuildOutput (([IO.File]::ReadAllText("$taskExplicit\shaders\amd-tuning.json")) -ceq '{"fixture":"direct"}') 'Explicit output omitted the preferred tuning cache'
  # Remove only this known fixture file; real source and GPU assets are never
  # involved. A checkout without the newer cache must still build the old one.
  Remove-Item -LiteralPath $taskPreferredTuning
  $taskLegacyOutput = Join-Path $taskScratch 'legacy tuning output'
  & (Join-Path $taskScratch 'scripts\build_shaders.ps1') -Backend amd -OutputDirectory $taskLegacyOutput
  Assert-TaskBuildOutput (([IO.File]::ReadAllText("$taskLegacyOutput\shaders\amd-tuning.json")) -ceq '{"fixture":true}') 'Missing direct cache did not fall back to legacy tuning'
  [IO.File]::WriteAllText($taskPreferredTuning, '{"fixture":"direct"}')
  $taskCount = $global:NrBuildOutputShaderCalls.Count
  & (Join-Path $taskScratch 'scripts\build.ps1') -Backend amd -SkipShaders -OutputDirectory (Join-Path $taskScratch 'skip shaders')
  Assert-TaskBuildOutput ($global:NrBuildOutputShaderCalls.Count -eq $taskCount) 'SkipShaders invoked shader compiler'
  $taskGame = Join-Path $taskScratch 'game experiment'
  $taskCount = $global:NrBuildOutputCompilerCalls.Count
  & (Join-Path $taskScratch 'scripts\build_game.ps1') -OutputDirectory $taskGame
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls.Count -eq $taskCount + 2) 'Game did not compile core and runtime once each'
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[$taskCount].Contains("/Fe:`"$taskGame\dlss5vk.exe`"")) 'Game did not forward base output directory to core'
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/OUT:`"$taskGame\game\OpenNrRuntime.dll`"")) 'Explicit runtime path ignored'
  Assert-TaskBuildOutput (Test-Path -LiteralPath "$taskGame\game\shaders\pack.spv") 'Game shader output path ignored'
  Assert-TaskBuildOutput (([IO.File]::ReadAllText("$taskGame\game\shaders\bridge.hlsl")) -eq '// compiler is mocked') 'Bridge HLSL copy missing'
  $taskCount = $global:NrBuildOutputCompilerCalls.Count
  & (Join-Path $taskScratch 'scripts\build_game.ps1') -SkipCore -OutputDirectory $taskGame
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls.Count -eq $taskCount + 1) 'SkipCore compiled core'
  & (Join-Path $taskScratch 'scripts\build_game.ps1') -SkipCore
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/OUT:`"$taskDefault\game\OpenNrRuntime.dll`"")) 'Default runtime path changed'
  & (Join-Path $taskScratch 'scripts\build_interop.ps1')
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/Fe:`"$taskDefault\interop\interop_selftest.exe`"")) 'Default interop path changed'
  $taskInterop = Join-Path $taskScratch 'interop experiment'
  & (Join-Path $taskScratch 'scripts\build_interop.ps1') -OutputDirectory $taskInterop
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/Fe:`"$taskInterop\interop\interop_selftest.exe`"")) 'Explicit interop executable path ignored'
  Assert-TaskBuildOutput ($global:NrBuildOutputCompilerCalls[-1].Contains("/Fo`"$taskInterop\interop\obj\\`"")) 'Explicit interop object path ignored'
  foreach ($taskObject in @('vk_context', 'kernels', 'nr_graph', 'nr_model', 'volk')) { [IO.File]::WriteAllText("$taskInterop\interop\obj\$taskObject.obj", 'mock object bytes') }
  $null = [IO.Directory]::CreateDirectory("$taskInterop\game")
  [IO.File]::WriteAllText("$taskInterop\game\OpenNrRuntime.dll", 'mock runtime bytes')
  [IO.File]::WriteAllText("$taskGame\game\OpenNrRuntime.dll", 'mock alternate runtime bytes')
  $taskAssets = Join-Path $taskScratch 'local assets'
  $null = [IO.Directory]::CreateDirectory("$taskAssets\model")
  [IO.File]::WriteAllText("$taskAssets\model\manifest.json", '{}')
  [Environment]::SetEnvironmentVariable('OPEN_NR_RUNTIME_DLL', 'preserved-parent-dll', 'Process')
  [Environment]::SetEnvironmentVariable('OPEN_NR_RUNTIME_FRAMES', 'preserved-parent-frames', 'Process')
  & (Join-Path $taskScratch 'scripts\build_interop.ps1') -SkipCore -Run -OutputDirectory $taskInterop -RuntimeAssets $taskAssets -Frames 8 -Trace -Capture -CaptureFrames 2
  $taskCall = $global:NrBuildOutputRuntimeCalls[-1]
  Assert-TaskBuildOutput ($taskCall.dll -eq "$taskInterop\game\OpenNrRuntime.dll") 'Isolated runtime DLL not forwarded'
  Assert-TaskBuildOutput ($taskCall.assets -eq $taskAssets -and $taskCall.enabled -eq '1') 'Local model assets not forwarded'
  Assert-TaskBuildOutput ($taskCall.frames -eq '8' -and $taskCall.trace -eq '1' -and $taskCall.capture -eq '1' -and $taskCall.capture_frames -eq '2') 'Runtime workload options ignored'
  Assert-TaskBuildOutput ($taskCall.repository -eq $taskScratch) 'Harness repository path changed'
  Assert-TaskBuildOutput ($env:OPEN_NR_RUNTIME_DLL -eq 'preserved-parent-dll' -and $env:OPEN_NR_RUNTIME_FRAMES -eq 'preserved-parent-frames') 'Runtime parent environment not restored'
  & (Join-Path $taskScratch 'scripts\build_interop.ps1') -SkipCore -Run -OutputDirectory $taskInterop -RuntimeAssets $taskAssets -RuntimeDll "$taskGame\game\OpenNrRuntime.dll"
  Assert-TaskBuildOutput ($global:NrBuildOutputRuntimeCalls[-1].dll -eq "$taskGame\game\OpenNrRuntime.dll") 'Explicit runtime DLL override ignored'
  $taskRejected = $false
  try { & (Join-Path $taskScratch 'scripts\build_interop.ps1') -SkipCore -Run -OutputDirectory $taskInterop -RuntimeDll (Join-Path $taskScratch 'missing.dll') } catch { $taskRejected = $true }
  Assert-TaskBuildOutput $taskRejected 'Missing explicit runtime DLL accepted'
  Assert-TaskBuildOutput ($env:OPEN_NR_RUNTIME_DLL -eq 'preserved-parent-dll') 'Missing DLL changed caller environment'
  $taskRejected = $false
  try { & (Join-Path $taskScratch 'scripts\build_interop.ps1') -SkipCore -Run -OutputDirectory $taskInterop -RuntimeAssets (Join-Path $taskScratch 'missing-assets') } catch { $taskRejected = $true }
  Assert-TaskBuildOutput $taskRejected 'Missing runtime assets accepted'
  Assert-TaskBuildOutput ($env:OPEN_NR_RUNTIME_DLL -eq 'preserved-parent-dll' -and $env:OPEN_NR_RUNTIME_FRAMES -eq 'preserved-parent-frames') 'Asset failure did not restore caller environment'
  foreach ($taskInvocation in $global:NrBuildOutputShaderCalls) {
    $taskIndex = [Array]::IndexOf($taskInvocation, '-o')
    $taskTarget = [IO.Path]::GetFullPath([string]$taskInvocation[$taskIndex + 1])
    Assert-TaskBuildOutput ($taskTarget.StartsWith($taskScratch + '\', [StringComparison]::OrdinalIgnoreCase)) 'Mock shader destination escaped fresh workspace'
  }
  Write-Host "BUILD OUTPUT TEST PASS: $taskChecks CPU checks; no compiler or GPU invoked. Workspace: $taskScratch"
} finally {
  Remove-Item -LiteralPath $taskFakeFunction
  Remove-Item -LiteralPath $taskInteropFunction
  if ($taskOldCmd) { Set-Item -LiteralPath 'Function:\cmd' -Value $taskOldCmd.ScriptBlock } else { Remove-Item -LiteralPath 'Function:\cmd' }
  foreach ($taskName in $taskRuntimeEnvironmentNames) { [Environment]::SetEnvironmentVariable($taskName, $taskOriginalRuntimeEnvironment[$taskName], 'Process') }
  Remove-Variable -Name NrBuildOutputShaderCalls, NrBuildOutputCompilerCalls, NrBuildOutputRuntimeCalls -Scope Global
}
