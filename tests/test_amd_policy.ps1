# CPU-only AMD session selectors and tuning/evidence validation. No GPU work.
param([string]$TuningFile)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $TuningFile) { $TuningFile = Join-Path $taskRoot 'docs\performance\rx9070xt-26.9.1-k16.json' }
$TuningFile = (Resolve-Path -LiteralPath $TuningFile -ErrorAction Stop).Path
$taskTuningMetadata = Get-Content -LiteralPath $TuningFile -Raw | ConvertFrom-Json
$taskExpectedGemm = if ($taskTuningMetadata.default_selection.PSObject.Properties['gemm']) { $taskTuningMetadata.default_selection.gemm } else { 'shared' }
if ($taskExpectedGemm -notin @('shared','packed','direct')) { throw 'Audited tuning names an invalid GEMM policy' }
$taskOutput = Join-Path (Join-Path $taskRoot 'build\fusion-policy-cpu') ([Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $taskOutput -Force
$taskVcvars = & (Join-Path $taskRoot 'scripts\find_vcvars.ps1')
$taskChecks = [ordered]@{}
foreach ($taskTest in @('amd_config_tests','amd_selection_tests')) {
  $taskSource = Join-Path $taskRoot ('tests\' + $taskTest + '.cpp')
  $taskCommand = '"{0}" >nul && cl /nologo /std:c++20 /EHsc /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /Fo"{1}/{2}.obj" /Fe:"{1}/{2}.exe" "{3}"' -f $taskVcvars,$taskOutput,$taskTest,$taskSource
  cmd /c $taskCommand 2>&1 | Tee-Object -FilePath (Join-Path $taskOutput ($taskTest + '-compile.log'))
  if ($LASTEXITCODE -ne 0) { throw "CPU policy test compilation failed: $taskTest" }
  $taskArguments = @()
  if ($taskTest -eq 'amd_selection_tests') { $taskArguments += @($TuningFile, $taskExpectedGemm) }
  $taskResults = @(& (Join-Path $taskOutput ($taskTest + '.exe')) @taskArguments 2>&1 | Tee-Object -FilePath (Join-Path $taskOutput ($taskTest + '.log')))
  $taskResults | Write-Output
  if ($LASTEXITCODE -ne 0) { throw "CPU policy regression failed: $taskTest" }
  $taskMatch = [regex]::Match(($taskResults -join "`n"),'AMD (?:process configuration|selection lifetime): ([0-9]+) CPU checks PASS')
  if (-not $taskMatch.Success) { throw "CPU test result is missing: $taskTest" }
  $taskChecks[$taskTest] = [int]$taskMatch.Groups[1].Value
}
$taskInputs = @('src/amd_config.h','src/amd_selection.h','src/amd_qualified_fallback.h','src/json.h','tests/amd_config_tests.cpp','tests/amd_selection_tests.cpp','tests/test_amd_policy.ps1')
$taskHashes = foreach ($taskInput in $taskInputs) {
  $taskPath = Join-Path $taskRoot $taskInput
  [ordered]@{ file = $taskInput; length = (Get-Item -LiteralPath $taskPath).Length; sha256 = (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant() }
}
[ordered]@{
  format = 'OpenNR-amd-policy-cpu-v1'
  recordedUtc = [DateTime]::UtcNow.ToString('o')
  gpuExecution = $false
  checks = $taskChecks
  passed = $true
  sourceInputs = @($taskHashes)
  auditedTuning = [ordered]@{ sha256 = (Get-FileHash -LiteralPath $TuningFile -Algorithm SHA256).Hash.ToLowerInvariant(); expectedGemm = $taskExpectedGemm; fullPolicyAndRecordsChecked = $true }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput 'verification.json') -Encoding utf8
Write-Host "CPU-only AMD policy checks passed; private proof: $taskOutput\verification.json"
exit 0
