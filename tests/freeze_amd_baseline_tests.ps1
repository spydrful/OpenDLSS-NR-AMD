# CPU-only tests of the actual source-identity function; the snapshot's device
# query and its executable/compiler paths are never invoked here.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskScript=Join-Path $taskRoot 'scripts\freeze_amd_baseline.ps1'
$taskTokens=$null;$taskErrors=$null
$taskAst=[Management.Automation.Language.Parser]::ParseFile($taskScript,[ref]$taskTokens,[ref]$taskErrors)
if($taskErrors.Count){throw 'Baseline snapshot script has parser errors'}
$taskFunction=$taskAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-NrFrozenSourceIdentity'},$false)
if(-not $taskFunction){throw 'Source identity function not found'}
. ([scriptblock]::Create($taskFunction.Extent.Text))
$taskScratch=Join-Path $taskRoot ('build\freeze-source-tests-'+[Guid]::NewGuid().ToString('N'))
$taskChecks=0
function Assert-Nr([bool]$Condition,[string]$Message){++$script:taskChecks;if(-not $Condition){throw $Message}}
try{
  New-Item -ItemType Directory -Path (Join-Path $taskScratch 'src'),(Join-Path $taskScratch 'shaders'),(Join-Path $taskScratch 'scripts'),(Join-Path $taskScratch 'src\build'),(Join-Path $taskScratch 'tests\fixtures'),(Join-Path $taskScratch 'models') -Force | Out-Null
  $taskSource=Join-Path $taskScratch 'src\unit.cpp'
  Set-Content -LiteralPath $taskSource -Value '// source A' -NoNewline
  Set-Content -LiteralPath (Join-Path $taskScratch 'shaders\unit.comp') -Value '// shader A' -NoNewline
  Set-Content -LiteralPath (Join-Path $taskScratch 'scripts\unit.ps1') -Value '# script A' -NoNewline
  foreach($taskExcluded in @('src\unit.dll','src\weights.bin','src\build\generated.cpp','tests\fixtures\capture.json','models\manifest.json')){
    Set-Content -LiteralPath (Join-Path $taskScratch $taskExcluded) -Value 'excluded local/proprietary/generated input'
  }
  $taskFrozen=Get-NrFrozenSourceIdentity $taskScratch
  $taskFrozenJson=$taskFrozen | ConvertTo-Json -Depth 6
  Assert-Nr ($taskFrozen.files.Count -eq 3) 'source closure included binary/model/generated inputs or omitted source'
  Assert-Nr ($taskFrozen.sha256 -match '^[0-9a-f]{64}$') 'source closure hash is not SHA-256'
  Assert-Nr (@($taskFrozen.files | Where-Object {$_.path -like '*\*'}).Count -eq 0) 'source identity paths are not portable'
  Assert-Nr (($taskFrozen.files | Where-Object {$_.path -eq 'src/unit.cpp'}).sha256 -eq (Get-FileHash -LiteralPath $taskSource -Algorithm SHA256).Hash.ToLowerInvariant()) 'source hash does not bind actual bytes'
  Assert-Nr (-not $taskFrozen.sourceBytesCopied -and -not $taskFrozen.binaryCorrespondenceEstablished) 'hash metadata falsely claims source archive or binary reproduction'
  Assert-Nr ((Get-NrFrozenSourceIdentity $taskScratch).sha256 -eq $taskFrozen.sha256) 'unchanged source closure is not reproducible'
  Set-Content -LiteralPath $taskSource -Value '// source B' -NoNewline
  $taskChanged=Get-NrFrozenSourceIdentity $taskScratch
  Assert-Nr ($taskChanged.sha256 -ne $taskFrozen.sha256) 'modified source did not change closure identity'
  Assert-Nr (($taskFrozen | ConvertTo-Json -Depth 6) -ceq $taskFrozenJson) 'later source edits mutated the frozen identity object'
  Assert-Nr (($taskChanged.files | Where-Object {$_.path -eq 'shaders/unit.comp'}).sha256 -eq ($taskFrozen.files | Where-Object {$_.path -eq 'shaders/unit.comp'}).sha256) 'unrelated shader identity changed'
  Write-Host "AMD frozen source identity: $taskChecks CPU checks PASS (no device query or compiler/GPU invocation)."
}finally{
  $taskResolved=[IO.Path]::GetFullPath($taskScratch)
  $taskExpected=[IO.Path]::GetFullPath((Join-Path $taskRoot 'build')).TrimEnd('\')+'\freeze-source-tests-'
  if(-not $taskResolved.StartsWith($taskExpected,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing unsafe freeze test cleanup'}
  if(Test-Path -LiteralPath $taskResolved){Remove-Item -LiteralPath $taskResolved -Recurse -Force}
}
