# CPU-only strict JSON/policy regression. Never builds or runs the GPU runtime.
param([string]$ModelManifest, [string]$RejectFile)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutputRoot = Join-Path $taskRoot 'build\json-parser-tests'
$taskOutput = Join-Path $taskOutputRoot ([Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $taskOutput -Force
$taskVcvars = & (Join-Path $taskRoot 'scripts\find_vcvars.ps1')
$taskCompilerPaths = @(cmd /c ('"' + $taskVcvars + '" >nul && where cl'))
if ($LASTEXITCODE -ne 0 -or $taskCompilerPaths.Count -eq 0) { throw 'Cannot locate the selected MSVC compiler' }
$taskCompiler = [IO.Path]::GetFullPath([string]$taskCompilerPaths[0])
$taskSource = Join-Path $taskRoot 'tests\json_parser_tests.cpp'
$taskExe = Join-Path $taskOutput 'json_parser_tests.exe'
$taskCommand = '"{0}" >nul && cl /nologo /std:c++20 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /Fo"{1}/json_parser_tests.obj" /Fe:"{2}" "{3}"' -f $taskVcvars,$taskOutput,$taskExe,$taskSource
cmd /c $taskCommand 2>&1 | Tee-Object -FilePath (Join-Path $taskOutput 'compile.log')
if ($LASTEXITCODE -ne 0) { throw 'JSON CPU test compilation failed' }
$taskTuning = Join-Path $taskRoot 'docs\performance\rx9070xt-26.9.1-k16.json'
$taskTuningSource = [IO.File]::ReadAllText($taskTuning)
$taskFixturePattern = '"optimized_default_eligible"\s*:\s*true'
$taskFixtureMatches = [regex]::Matches($taskTuningSource, $taskFixturePattern)
if ($taskFixtureMatches.Count -ne 1) { throw 'Expected exactly one optimized_default_eligible true field in the shipped tuning cache' }
$taskFixtureSource = [regex]::Replace($taskTuningSource, $taskFixturePattern, '"optimized_default_eligible": trux')
$taskFixture = Join-Path $taskOutput 'malformed-literal-tuning.json'
[IO.File]::WriteAllText($taskFixture, $taskFixtureSource, [Text.UTF8Encoding]::new($false))
$taskArguments = @('--tuning', $taskTuning, '--reject', $taskFixture, '--fallback', $taskFixture)
if ($ModelManifest) { $ModelManifest = [IO.Path]::GetFullPath($ModelManifest); $taskArguments += @('--model', $ModelManifest) }
if ($RejectFile) { $RejectFile = [IO.Path]::GetFullPath($RejectFile); $taskArguments += @('--reject', $RejectFile, '--fallback', $RejectFile) }
$taskResults = @(& $taskExe @taskArguments 2>&1 | Tee-Object -FilePath (Join-Path $taskOutput 'tests.log'))
if ($LASTEXITCODE -ne 0) { throw 'JSON CPU regression failed' }
$taskMatch = [regex]::Match(($taskResults -join "`n"), 'JSON parser: ([0-9]+) CPU checks PASS')
if (-not $taskMatch.Success) { throw 'JSON CPU test result is missing' }
$taskInputs = @('src/json.h','src/amd_selection.h','src/amd_config.h','src/amd_qualified_fallback.h','tests/json_parser_tests.cpp','tests/test_json_parser.ps1')
$taskSourceHashes = foreach ($taskInput in $taskInputs) {
  $taskPath = Join-Path $taskRoot $taskInput
  [ordered]@{ file = $taskInput; length = (Get-Item -LiteralPath $taskPath).Length; sha256 = (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$taskCompilerInfo = Get-Item -LiteralPath $taskCompiler
$taskProof = [ordered]@{
  format = 'OpenNR-json-source-hardening-cpu-v1'
  recordedUtc = [DateTime]::UtcNow.ToString('o')
  scope = 'Post-alpha2 source hardening; CPU-only; existing GPU runtime/replay binaries and published assets unchanged'
  gpuExecution = $false
  compiler = [ordered]@{ path = $taskCompiler; length = $taskCompilerInfo.Length; sha256 = (Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash.ToLowerInvariant(); fileVersion = $taskCompilerInfo.VersionInfo.FileVersion }
  sourceInputs = @($taskSourceHashes)
  executable = [ordered]@{ file = 'json_parser_tests.exe'; sha256 = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLowerInvariant() }
  checks = [int]$taskMatch.Groups[1].Value
  passed = $true
  validTuningParsed = [ordered]@{ sha256 = (Get-FileHash -LiteralPath $taskTuning -Algorithm SHA256).Hash.ToLowerInvariant(); fullPolicyAndRecordsChecked = $true }
  defaultMalformedCache = [ordered]@{ file = 'malformed-literal-tuning.json'; sourceMatches = $taskFixtureMatches.Count; sha256 = (Get-FileHash -LiteralPath $taskFixture -Algorithm SHA256).Hash.ToLowerInvariant(); rejected = $true; preservingFallbackRetained = $true }
  optionalModelParsed = if ($ModelManifest) { [ordered]@{ sha256 = (Get-FileHash -LiteralPath $ModelManifest -Algorithm SHA256).Hash.ToLowerInvariant(); countsChecked = $true } } else { $null }
  optionalMalformedFile = if ($RejectFile) { [ordered]@{ sha256 = (Get-FileHash -LiteralPath $RejectFile -Algorithm SHA256).Hash.ToLowerInvariant(); rejected = $true; preservingFallbackRetained = $true } } else { $null }
}
$taskProof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput 'verification.json') -Encoding utf8
Write-Host "CPU-only JSON parser checks passed; private proof: $taskOutput\verification.json"
exit 0
