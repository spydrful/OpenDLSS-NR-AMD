# CPU-only closure/structure checks using an independently compiled SiLU module.
param([Parameter(Mandatory)][string]$ModulePath)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskTool = Join-Path $taskRoot 'scripts/complete_amd_silu_float_controls.ps1'
$taskOutput = Join-Path $taskRoot ('build/performance/silu-closure-cpu/' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $taskOutput -Force
$script:taskChecks = 0
function Assert-Task([bool]$Condition, [string]$Message) {
  if (!$Condition) { throw $Message }
  ++$script:taskChecks
}
function Read-TaskWords([string]$Path) {
  $taskData = [IO.File]::ReadAllBytes($Path)
  [uint32[]]$taskResult = [uint32[]]::new($taskData.Length / 4)
  for ($i = 0; $i -lt $taskResult.Length; ++$i) { $taskResult[$i] = [BitConverter]::ToUInt32($taskData, $i * 4) }
  return ,$taskResult
}
function Write-TaskWords([string]$Path, [uint32[]]$Words) {
  $taskData = [byte[]]::new($Words.Length * 4)
  for ($i = 0; $i -lt $Words.Length; ++$i) { [Buffer]::BlockCopy([BitConverter]::GetBytes($Words[$i]), 0, $taskData, $i * 4, 4) }
  [IO.File]::WriteAllBytes($Path, $taskData)
}
function Get-TaskInstructions([uint32[]]$Words) {
  for ($i = 5; $i -lt $Words.Length;) {
    $taskCount = [int]($Words[$i] -shr 16)
    if (!$taskCount) { throw 'Bad CPU fixture' }
    [pscustomobject]@{ offset=$i; count=$taskCount; op=($Words[$i] -band 65535) }
    $i += $taskCount
  }
}
function Remove-TaskInstruction([uint32[]]$Words, $Instruction) {
  $taskResult = [uint32[]]::new($Words.Length - $Instruction.count)
  [Array]::Copy($Words, 0, $taskResult, 0, $Instruction.offset)
  [Array]::Copy($Words, $Instruction.offset + $Instruction.count, $taskResult, $Instruction.offset,
                $Words.Length - $Instruction.offset - $Instruction.count)
  return ,$taskResult
}
function Insert-TaskInstruction([uint32[]]$Words, [int]$Offset, [uint32[]]$Instruction) {
  $taskResult = [uint32[]]::new($Words.Length + $Instruction.Length)
  [Array]::Copy($Words, 0, $taskResult, 0, $Offset)
  [Array]::Copy($Instruction, 0, $taskResult, $Offset, $Instruction.Length)
  [Array]::Copy($Words, $Offset, $taskResult, $Offset + $Instruction.Length, $Words.Length - $Offset)
  return ,$taskResult
}
function Test-TaskReject([string]$Name, [uint32[]]$Words) {
  $taskInput = Join-Path $taskOutput ($Name + '.spv')
  $taskDest = Join-Path $taskOutput ($Name + '-output.spv')
  Write-TaskWords $taskInput $Words
  [IO.File]::WriteAllBytes($taskDest, [byte[]]@(91,92,93))
  $taskBefore = (Get-FileHash -LiteralPath $taskDest -Algorithm SHA256).Hash
  $taskRejected = $false
  try { $null = & $taskTool -InputPath $taskInput -OutputPath $taskDest }
  catch { $taskRejected = $true }
  Assert-Task $taskRejected ("Closure accepted bad fixture: $Name")
  Assert-Task ((Get-FileHash -LiteralPath $taskDest -Algorithm SHA256).Hash -eq $taskBefore) ("Closure modified output on rejection: $Name")
}
$taskFixed = Join-Path $taskOutput 'complete.spv'
$taskInitial = & $taskTool -InputPath $ModulePath -OutputPath $taskFixed
[uint32[]]$taskBase = Read-TaskWords $taskFixed
$taskInstructions = @(Get-TaskInstructions $taskBase)
$taskSigned16 = $taskInstructions | Where-Object { $_.op -eq 16 -and $taskBase[$_.offset + 2] -eq 4461 -and $taskBase[$_.offset + 3] -eq 16 }
$taskSigned32 = $taskInstructions | Where-Object { $_.op -eq 16 -and $taskBase[$_.offset + 2] -eq 4461 -and $taskBase[$_.offset + 3] -eq 32 }
Assert-Task ($null -ne $taskSigned16 -and $null -ne $taskSigned32) 'Both signed-zero modes are required'
$taskFirstHash = (Get-FileHash -LiteralPath $taskFixed -Algorithm SHA256).Hash
$taskAgain = & $taskTool -InputPath $taskFixed -OutputPath $taskFixed
Assert-Task ($taskAgain.added_signed_zero_widths.Count -eq 0) 'Closure is not idempotent'
Assert-Task ((Get-FileHash -LiteralPath $taskFixed -Algorithm SHA256).Hash -eq $taskFirstHash) 'Idempotent closure changed bytes'
foreach ($taskWidth in @(16,32)) {
  $taskRemoved = if ($taskWidth -eq 16) { $taskSigned16 } else { $taskSigned32 }
  [uint32[]]$taskOneWidth = Remove-TaskInstruction $taskBase $taskRemoved
  $taskOnePath = Join-Path $taskOutput ("missing-$taskWidth.spv")
  Write-TaskWords $taskOnePath $taskOneWidth
  $taskClosed = & $taskTool -InputPath $taskOnePath -OutputPath $taskOnePath
  Assert-Task ($taskClosed.added_signed_zero_widths.Count -eq 1 -and $taskClosed.added_signed_zero_widths[0] -eq $taskWidth) 'Wrong mode inserted'
  [uint32[]]$taskAfter = Read-TaskWords $taskOnePath
  Assert-Task ($taskAfter[3] -eq $taskOneWidth[3]) 'Closure changed ID bound'
  $taskAfterModes = @(Get-TaskInstructions $taskAfter)
  $taskInserted = $taskAfterModes | Where-Object { $_.op -eq 16 -and $taskAfter[$_.offset + 2] -eq 4461 -and $taskAfter[$_.offset + 3] -eq $taskWidth }
  [uint32[]]$taskStripped = Remove-TaskInstruction $taskAfter $taskInserted
  $taskEqual = [Linq.Enumerable]::SequenceEqual([uint32[]]$taskOneWidth, [uint32[]]$taskStripped)
  Assert-Task $taskEqual 'Closure changed original instructions'
}
foreach ($taskCase in @('magic','version','schema','bound')) {
  [uint32[]]$taskBad = $taskBase.Clone()
  switch ($taskCase) { 'magic' {$taskBad[0]=0}; 'version' {$taskBad[1]=0x00010500}; 'schema' {$taskBad[4]=1}; 'bound' {$taskBad[3]=0} }
  Test-TaskReject $taskCase $taskBad
}
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[5] = 17
Test-TaskReject 'zero-instruction-size' $taskBad
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskBad.Length - 1] = 0x00020038
Test-TaskReject 'truncated-instruction' $taskBad
$taskEntry = $taskInstructions | Where-Object op -eq 15
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskEntry.offset + 1] = 0
Test-TaskReject 'wrong-stage' $taskBad
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskEntry.offset + 2] = $taskBad[3]
Test-TaskReject 'entry-id-bound' $taskBad
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskEntry.offset + 3] = [uint32]::MaxValue
Test-TaskReject 'entry-name' $taskBad
[uint32[]]$taskDuplicate = $taskBase[$taskEntry.offset..($taskEntry.offset + $taskEntry.count - 1)]
Test-TaskReject 'duplicate-entry' (Insert-TaskInstruction $taskBase $taskEntry.offset $taskDuplicate)
$taskLocalSize = $taskInstructions | Where-Object { $_.op -eq 16 -and $taskBase[$_.offset + 2] -eq 17 }
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskLocalSize.offset + 3] = 0
Test-TaskReject 'zero-local-size' $taskBad
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskSigned32.offset + 1] = $taskBad[3]
Test-TaskReject 'mode-entry-id' $taskBad
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskSigned32.offset + 3] = 64
Test-TaskReject 'signed-width' $taskBad
[uint32[]]$taskDuplicate = $taskBase[$taskSigned32.offset..($taskSigned32.offset + $taskSigned32.count - 1)]
Test-TaskReject 'duplicate-mode' (Insert-TaskInstruction $taskBase $taskSigned32.offset $taskDuplicate)
foreach ($taskCap in @(1,9,4464,4466,4467)) {
  $taskInstruction = $taskInstructions | Where-Object { $_.op -eq 17 -and $taskBase[$_.offset + 1] -eq $taskCap }
  Test-TaskReject ("missing-cap-$taskCap") (Remove-TaskInstruction $taskBase $taskInstruction)
}
$taskCapability = $taskInstructions | Where-Object { $_.op -eq 17 -and $taskBase[$_.offset + 1] -eq 4466 }
Test-TaskReject 'duplicate-cap' (Insert-TaskInstruction $taskBase $taskCapability.offset ([uint32[]]@(0x00020011,4466)))
foreach ($taskMode in @(4459,4462)) {
  $taskInstruction = $taskInstructions | Where-Object { $_.op -eq 16 -and $taskBase[$_.offset + 2] -eq $taskMode }
  Test-TaskReject ("missing-mode-$taskMode") (Remove-TaskInstruction $taskBase $taskInstruction)
  [uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskInstruction.offset + 3] = 32
  Test-TaskReject ("unsupported-width-$taskMode") $taskBad
}
[uint32[]]$taskNo16 = Remove-TaskInstruction $taskBase $taskSigned16
$taskNo16Instructions = @(Get-TaskInstructions $taskNo16)
$taskNo16Signed32 = $taskNo16Instructions | Where-Object { $_.op -eq 16 -and $taskNo16[$_.offset + 2] -eq 4461 }
Test-TaskReject 'missing-all-signed-modes' (Remove-TaskInstruction $taskNo16 $taskNo16Signed32)
$taskMemory = $taskInstructions | Where-Object op -eq 14
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskMemory.offset + 1] = 1
Test-TaskReject 'memory-model' $taskBad
Test-TaskReject 'missing-memory-model' (Remove-TaskInstruction $taskBase $taskMemory)
$taskFloat = $taskInstructions | Where-Object { $_.op -eq 22 -and $taskBase[$_.offset + 2] -eq 32 } | Select-Object -First 1
[uint32[]]$taskBad = $taskBase.Clone(); $taskBad[$taskFloat.offset + 2] = 64
Test-TaskReject 'missing-float32-type' $taskBad
$taskLastEnd = $taskInstructions | Where-Object op -eq 56 | Select-Object -Last 1
Test-TaskReject 'missing-function-end' (Remove-TaskInstruction $taskBase $taskLastEnd)
Assert-Task (!(Get-ChildItem -LiteralPath $taskOutput -Filter '*.tmp' -Force)) 'Closure leaked temporary files'
[ordered]@{
  format='OpenNR-amd-silu-closure-cpu-v1'; gpu_execution=$false; passed=$true; checks=$script:taskChecks
  input_sha256=(Get-FileHash -LiteralPath $ModulePath -Algorithm SHA256).Hash.ToLowerInvariant()
  closure_tool_sha256=(Get-FileHash -LiteralPath $taskTool -Algorithm SHA256).Hash.ToLowerInvariant()
  test_script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
  complete_module_sha256=$taskFirstHash.ToLowerInvariant()
  scope='Structural contract, exact insertion, both missing widths, idempotence and fail-closed output preservation; no semantic validator or GPU execution.'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOutput 'verification.json') -Encoding utf8
Write-Host "AMD SiLU float-control closure: $script:taskChecks CPU checks PASS; proof: $taskOutput/verification.json"
