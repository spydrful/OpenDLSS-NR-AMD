# Close only the scalar-RTE SiLU module's declared F16/F32 zero/Inf/NaN modes.
# glslang's intrinsic map retains one value per execution-mode enum and can
# discard one width. Insert the missing OpExecutionMode without rewriting code.
# This checks structure and this module contract, not full SPIR-V semantics.
param(
  [Parameter(Mandatory)][string]$InputPath,
  [Parameter(Mandatory)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$taskInput = [IO.Path]::GetFullPath($InputPath)
$taskOutput = [IO.Path]::GetFullPath($OutputPath)
$taskBytes = [IO.File]::ReadAllBytes($taskInput)
if ($taskBytes.Length -lt 20 -or $taskBytes.Length -gt 32MB -or $taskBytes.Length % 4) {
  throw 'Unsupported SPIR-V byte length'
}
[uint32[]]$taskWords = [uint32[]]::new($taskBytes.Length / 4)
for ($i = 0; $i -lt $taskWords.Length; ++$i) { $taskWords[$i] = [BitConverter]::ToUInt32($taskBytes, $i * 4) }
if ($taskWords[0] -ne 0x07230203 -or $taskWords[1] -ne 0x00010600 -or !$taskWords[3] -or $taskWords[4]) {
  throw 'Expected a SPIR-V 1.6 module with a valid header'
}
$taskBound = $taskWords[3]
function Test-TaskId([uint32]$Value) { return $Value -gt 0 -and $Value -lt $taskBound }
$taskCaps = @{}
$taskModes = @{}
$taskEntry = $null
$taskMemoryModels = 0
$taskFloat16 = $false
$taskFloat32 = $false
$taskFunctions = @{}
$taskInFunction = $false
$taskFunctionSeen = $false
$taskDeclarationSeen = $false
$taskInsert = 0
for ($i = 5; $i -lt $taskWords.Length;) {
  $taskSize = [int]($taskWords[$i] -shr 16)
  $taskOp = [int]($taskWords[$i] -band 65535)
  if (!$taskSize -or $i + $taskSize -gt $taskWords.Length -or !$taskOp) { throw 'Malformed SPIR-V instruction stream' }
  switch ($taskOp) {
    17 { # OpCapability
      if ($taskSize -ne 2 -or $taskFunctionSeen -or $taskDeclarationSeen) { throw 'Malformed capability declaration' }
      $taskCap = $taskWords[$i + 1]
      if ($taskCaps.ContainsKey($taskCap)) { throw 'Duplicate SPIR-V capability' }
      $taskCaps[$taskCap] = $true
    }
    14 { # OpMemoryModel: the two logical models used by these compute modules
      if ($taskSize -ne 3 -or $taskWords[$i + 1] -ne 0 -or $taskWords[$i + 2] -notin @(1,3) -or $taskFunctionSeen) {
        throw 'Unsupported memory model'
      }
      ++$taskMemoryModels
    }
    15 { # OpEntryPoint
      if ($taskSize -lt 5 -or $taskEntry -or $taskFunctionSeen -or $taskWords[$i + 1] -ne 5 -or !(Test-TaskId $taskWords[$i + 2])) {
        throw 'Expected exactly one compute entry point'
      }
      $taskNameBytes = [Collections.Generic.List[byte]]::new()
      $taskNameEnd = -1
      for ($j = $i + 3; $j -lt $i + $taskSize -and $taskNameEnd -lt 0; ++$j) {
        for ($k = 0; $k -lt 4; ++$k) {
          $taskByte = [byte](($taskWords[$j] -shr ($k * 8)) -band 255)
          if (!$taskByte) {
            for ($n = $k + 1; $n -lt 4; ++$n) {
              if (($taskWords[$j] -shr ($n * 8)) -band 255) { throw 'Malformed entry-point name padding' }
            }
            $taskNameEnd = $j + 1
            break
          }
          $taskNameBytes.Add($taskByte)
        }
      }
      if ($taskNameEnd -lt 0 -or [Text.Encoding]::UTF8.GetString($taskNameBytes.ToArray()) -ne 'main') {
        throw 'Expected the main compute entry point'
      }
      for ($j = $taskNameEnd; $j -lt $i + $taskSize; ++$j) {
        if (!(Test-TaskId $taskWords[$j])) { throw 'Entry-point interface ID exceeds the module bound' }
      }
      $taskEntry = $taskWords[$i + 2]
    }
    16 { # OpExecutionMode
      if (!$taskEntry -or $taskFunctionSeen -or $taskDeclarationSeen -or $taskSize -lt 4 -or $taskWords[$i + 1] -ne $taskEntry) {
        throw 'Malformed execution-mode declaration'
      }
      $taskMode = $taskWords[$i + 2]
      if ($taskMode -eq 17) {
        if ($taskSize -ne 6 -or !$taskWords[$i + 3] -or !$taskWords[$i + 4] -or !$taskWords[$i + 5]) {
          throw 'Expected a literal, nonzero compute local size'
        }
        $taskKey = 'local-size'
      } elseif ($taskMode -in @(4459,4461,4462)) {
        if ($taskSize -ne 4) { throw 'Malformed float-control execution mode' }
        $taskWidth = $taskWords[$i + 3]
        if (($taskMode -eq 4461 -and $taskWidth -notin @(16,32)) -or ($taskMode -ne 4461 -and $taskWidth -ne 16)) {
          throw 'Unsupported float-control width or policy'
        }
        $taskKey = "$taskMode/$taskWidth"
      } else { throw 'Unsupported execution mode for the SiLU closure tool' }
      if ($taskModes.ContainsKey($taskKey)) { throw 'Duplicate execution mode' }
      $taskModes[$taskKey] = $true
      $taskInsert = $i + $taskSize
    }
    331 { throw 'ExecutionModeId is unsupported by the SiLU closure tool' }
    22 { # OpTypeFloat
      $taskDeclarationSeen = $true
      if ($taskSize -lt 3 -or !(Test-TaskId $taskWords[$i + 1])) { throw 'Malformed float type' }
      if ($taskWords[$i + 2] -eq 16) { $taskFloat16 = $true }
      if ($taskWords[$i + 2] -eq 32) { $taskFloat32 = $true }
    }
    54 { # OpFunction
      $taskFunctionSeen = $true
      if ($taskSize -ne 5 -or $taskInFunction -or !(Test-TaskId $taskWords[$i + 1]) -or
          !(Test-TaskId $taskWords[$i + 2]) -or !(Test-TaskId $taskWords[$i + 4])) { throw 'Malformed function declaration' }
      $taskFunctionId = $taskWords[$i + 2]
      if ($taskFunctions.ContainsKey($taskFunctionId)) { throw 'Duplicate function ID' }
      $taskFunctions[$taskFunctionId] = $true
      $taskInFunction = $true
    }
    56 {
      if ($taskSize -ne 1 -or !$taskInFunction) { throw 'Malformed function end' }
      $taskInFunction = $false
    }
  }
  $i += $taskSize
}
if ($taskMemoryModels -ne 1 -or !$taskEntry -or $taskInFunction -or !$taskFunctions.ContainsKey($taskEntry) -or
    !$taskFloat16 -or !$taskFloat32 -or !$taskModes.ContainsKey('local-size') -or
    !$taskModes.ContainsKey('4459/16') -or !$taskModes.ContainsKey('4462/16') -or
    (!$taskModes.ContainsKey('4461/16') -and !$taskModes.ContainsKey('4461/32'))) {
  throw 'Module does not satisfy the scalar-RTE SiLU closure contract'
}
foreach ($taskCap in [uint32[]]@(1,9,4464,4466,4467)) {
  if (!$taskCaps.ContainsKey($taskCap)) { throw "Required SPIR-V capability $taskCap is missing" }
}
$taskAdded = @()
$taskNewWords = [Collections.Generic.List[uint32]]::new()
foreach ($taskWidth in @(16,32)) {
  if (!$taskModes.ContainsKey("4461/$taskWidth")) {
    $taskAdded += $taskWidth
    $taskNewWords.Add([uint32]0x00040010)
    $taskNewWords.Add([uint32]$taskEntry)
    $taskNewWords.Add([uint32]4461)
    $taskNewWords.Add([uint32]$taskWidth)
  }
}
$taskResult = [byte[]]::new($taskBytes.Length + $taskNewWords.Count * 4)
[Buffer]::BlockCopy($taskBytes, 0, $taskResult, 0, $taskInsert * 4)
for ($i = 0; $i -lt $taskNewWords.Count; ++$i) {
  [Buffer]::BlockCopy([BitConverter]::GetBytes($taskNewWords[$i]), 0, $taskResult, ($taskInsert + $i) * 4, 4)
}
[Buffer]::BlockCopy($taskBytes, $taskInsert * 4, $taskResult, ($taskInsert + $taskNewWords.Count) * 4, $taskBytes.Length - $taskInsert * 4)
if (![IO.Directory]::Exists([IO.Path]::GetDirectoryName($taskOutput))) { throw 'Output directory does not exist' }
$taskTemp = $null
try {
  if ($taskAdded.Count -or $taskInput -ne $taskOutput) {
    $taskTemp = Join-Path ([IO.Path]::GetDirectoryName($taskOutput)) ('.' + [IO.Path]::GetFileName($taskOutput) + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    [IO.File]::WriteAllBytes($taskTemp, $taskResult)
    if ([IO.File]::Exists($taskOutput)) { [IO.File]::Replace($taskTemp, $taskOutput, [NullString]::Value) }
    else { [IO.File]::Move($taskTemp, $taskOutput) }
  }
} finally {
  if ($taskTemp -and [IO.File]::Exists($taskTemp)) { [IO.File]::Delete($taskTemp) }
}
$taskSha = [Security.Cryptography.SHA256]::Create()
try {
  [pscustomobject]@{
    format = 'OpenNR-amd-silu-float-controls-closure-v1'
    input_sha256 = [BitConverter]::ToString($taskSha.ComputeHash($taskBytes)).Replace('-','').ToLowerInvariant()
    output_sha256 = [BitConverter]::ToString($taskSha.ComputeHash($taskResult)).Replace('-','').ToLowerInvariant()
    added_signed_zero_widths = @($taskAdded)
    entry_point_id = $taskEntry
    id_bound = $taskBound
    other_instructions_unchanged = $true
    semantic_validator_run = $false
  }
} finally { $taskSha.Dispose() }
