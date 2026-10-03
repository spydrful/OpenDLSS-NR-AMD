# CPU-only build and selector checks. Compiles SPIR-V; never creates a device.
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $taskRoot ('build\experimental-shader-cpu\' + [Guid]::NewGuid().ToString('N'))
}
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $taskOutput) { throw 'CPU shader proof output already exists' }
$null = New-Item -ItemType Directory -Path $taskOutput
$taskChecks = 0
function Assert-ShaderCpu([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
  $script:taskChecks++
}
function Get-ComputeModes([string]$Path) {
  $taskBytes = [IO.File]::ReadAllBytes($Path)
  if ($taskBytes.Length -lt 20 -or $taskBytes.Length % 4 -ne 0) { throw 'Invalid SPIR-V byte length' }
  $taskWords = [uint32[]]::new($taskBytes.Length / 4)
  [Buffer]::BlockCopy($taskBytes,0,$taskWords,0,$taskBytes.Length)
  if ($taskWords[0] -ne 0x07230203) { throw 'Invalid SPIR-V magic' }
  $taskModes = @()
  for ($taskOffset = 5; $taskOffset -lt $taskWords.Length;) {
    $taskCount = $taskWords[$taskOffset] -shr 16
    $taskOpcode = $taskWords[$taskOffset] -band 65535
    if ($taskCount -lt 1 -or $taskOffset + $taskCount -gt $taskWords.Length) { throw 'Invalid SPIR-V instruction' }
    if ($taskOpcode -eq 16 -and $taskCount -eq 4) {
      $taskModes += ('{0}:{1}' -f $taskWords[$taskOffset + 2], $taskWords[$taskOffset + 3])
    }
    $taskOffset += $taskCount
  }
  return $taskModes
}
$taskWrapper = Get-Command (Join-Path $taskRoot 'scripts\benchmark_amd.ps1')
$taskGemmSet = @($taskWrapper.Parameters['Gemm'].Attributes | Where-Object { $_ -is [Management.Automation.ValidateSetAttribute] })[0].ValidValues
$taskWindowSet = @($taskWrapper.Parameters['WindowLayout'].Attributes | Where-Object { $_ -is [Management.Automation.ValidateSetAttribute] })[0].ValidValues
Assert-ShaderCpu ($taskGemmSet -contains 'direct-rte-pair') 'Benchmark wrapper omits the pair selector'
Assert-ShaderCpu ($taskWindowSet -contains 'arena-rte') 'Benchmark wrapper omits the arena selector'
& (Join-Path $taskRoot 'scripts\build_shaders.ps1') -Backend amd -OutputDirectory $taskOutput *> (Join-Path $taskOutput 'compile.log')
if ($LASTEXITCODE -ne 0) { throw 'Native CPU shader build failed' }
$taskShaders = Join-Path $taskOutput 'shaders'
$taskModules = @()
foreach ($taskName in @('amd_gemm_direct_rte','amd_gemm_direct_rte_init','amd_gemm_direct_rte_epilogue',
                        'amd_gemm_direct_rte_pair','amd_window_register_rte','amd_window_arena_rte')) {
  $taskPath = Join-Path $taskShaders ($taskName + '.spv')
  Assert-ShaderCpu (Test-Path -LiteralPath $taskPath -PathType Leaf) ('Missing independent module: ' + $taskName)
  $taskModes = @(Get-ComputeModes $taskPath)
  foreach ($taskMode in @('4459:16','4461:16','4462:16')) {
    Assert-ShaderCpu ($taskModes -contains $taskMode) ('Missing half float control ' + $taskMode + ': ' + $taskName)
  }
  if ($taskName -in @('amd_gemm_direct_rte_epilogue','amd_gemm_direct_rte_pair')) {
    Assert-ShaderCpu ($taskModes -contains '4461:32') ('Missing SiLU F32 signed-zero control: ' + $taskName)
  }
  else {
    Assert-ShaderCpu ($taskModes -notcontains '4461:32') ('Unexpected F32 capability requirement: ' + $taskName)
  }
  $taskModules += [ordered]@{ name = $taskName; sha256 = (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant(); executionModes = $taskModes }
}
Assert-ShaderCpu ($taskModules[2].sha256 -ne $taskModules[3].sha256) 'Pair must have an independent SPIR-V identity'
Assert-ShaderCpu ($taskModules[4].sha256 -ne $taskModules[5].sha256) 'Arena must have an independent SPIR-V identity'
$taskFrozenTuning = Join-Path $taskRoot 'docs\performance\rx9070xt-26.9.1-rte-k16.json'
Assert-ShaderCpu ((Get-FileHash -LiteralPath $taskFrozenTuning -Algorithm SHA256).Hash -eq
                 (Get-FileHash -LiteralPath (Join-Path $taskShaders 'amd-tuning.json') -Algorithm SHA256).Hash) 'New modules changed the qualified default cache'
$taskInputs = foreach ($taskFile in @('scripts/build_shaders.ps1','scripts/benchmark_amd.ps1',
    'scripts/complete_amd_silu_float_controls.ps1','shaders/amd_gemm_direct_rte_pair.comp',
    'shaders/amd_window_arena_rte.comp','shaders/amd_silu_rte.glsl','shaders/amd_half_publication_rte.glsl',
    'shaders/portable_numeric.glsl','shaders/common.glsl','tests/test_amd_experimental_shader_build.ps1')) {
  $taskPath = Join-Path $taskRoot $taskFile
  [ordered]@{ file = $taskFile; sha256 = (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant() }
}
[ordered]@{
  format = 'OpenNR-amd-experimental-shader-cpu-v1'; gpuExecution = $false; passed = $true
  checks = $taskChecks; modules = $taskModules; sourceInputs = @($taskInputs)
  compiledModuleCount = @(Get-ChildItem -LiteralPath $taskShaders -Filter '*.spv').Count
  qualifiedCacheUnchanged = $true; semanticSpirvValidation = $false
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput 'verification.json') -Encoding utf8
Write-Host "AMD experimental shader build: $taskChecks CPU checks PASS; private proof $taskOutput\verification.json"
