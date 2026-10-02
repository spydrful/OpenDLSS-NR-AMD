$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskScratch = Join-Path $taskRoot ('build\static-source-tests-' + [Guid]::NewGuid().ToString('N'))
$taskChecks = 0
function Assert-Nr([bool]$Condition, [string]$Message) { ++$script:taskChecks; if (-not $Condition) { throw $Message } }
function Assert-NrThrows([scriptblock]$Action, [string]$Message) { ++$script:taskChecks; $taskThrew = $false; try { & $Action } catch { $taskThrew = $true }; if (-not $taskThrew) { throw $Message } }
try {
  New-Item -ItemType Directory -Path $taskScratch | Out-Null
  $taskArchive = Join-Path $taskRoot 'integrations\optiscaler\sources\fsr3-dx11-3.1.2.tar.gz'
  Assert-Nr ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash.ToLowerInvariant() -eq '0cf311b4095d0e9781b241b1ebaf205bb5768cce2a673220bbfcc1e76d0f9a52') 'FSR31 source archive differs from pin'
  $taskPrefix = 'FidelityFX-SDK-DX11-8138c9dc086154706643a03def91f3d01d391cd0'
  tar -xf $taskArchive -C $taskScratch --strip-components 1 ($taskPrefix + '/sdk/include/FidelityFX/host/backends/dx11/ffx_dx11.h') ($taskPrefix + '/sdk/src/backends/dx11/ffx_dx11.cpp')
  if ($LASTEXITCODE -ne 0) { throw 'Pinned source extraction failed' }
  $taskHeader = Join-Path $taskScratch 'sdk\include\FidelityFX\host\backends\dx11\ffx_dx11.h'
  $taskImpl = Join-Path $taskScratch 'sdk\src\backends\dx11\ffx_dx11.cpp'
  & (Join-Path $taskRoot 'scripts\patch_fsr31_static_source.ps1') -SourceDirectory $taskScratch
  Assert-Nr ((Get-FileHash -LiteralPath $taskHeader -Algorithm SHA256).Hash.ToLowerInvariant() -eq '95e1cd36ff33818ee0d096ed1c31db2011bf1d3eb751571c78a3254d0b13a2fe') 'Patched FSR31 header hash differs'
  Assert-Nr ((Get-FileHash -LiteralPath $taskImpl -Algorithm SHA256).Hash.ToLowerInvariant() -eq '6f70629c5b6a30a17059c8949eb271ccf9e145202787f33db18dd0da5cc39128') 'Patched FSR31 implementation hash differs'
  & (Join-Path $taskRoot 'scripts\patch_fsr31_static_source.ps1') -SourceDirectory $taskScratch
  Assert-Nr ((Get-FileHash -LiteralPath $taskHeader -Algorithm SHA256).Hash.ToLowerInvariant() -eq '95e1cd36ff33818ee0d096ed1c31db2011bf1d3eb751571c78a3254d0b13a2fe') 'Patch rerun was not idempotent'
  $taskImplHash = (Get-FileHash -LiteralPath $taskImpl -Algorithm SHA256).Hash
  Add-Content -LiteralPath $taskHeader -Value '// changed source'
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\patch_fsr31_static_source.ps1') -SourceDirectory $taskScratch } 'Different source was silently patched'
  Assert-Nr ((Get-FileHash -LiteralPath $taskImpl -Algorithm SHA256).Hash -eq $taskImplHash) 'Rejected source patch changed the other file'
  Write-Host "Static source: $taskChecks CPU checks passed; no library, GPU or game was used."
} finally {
  $taskResolved = [IO.Path]::GetFullPath($taskScratch)
  $taskAllowed = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')).TrimEnd('\') + '\static-source-tests-'
  if (-not $taskResolved.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing unsafe test cleanup path' }
  if (Test-Path -LiteralPath $taskResolved) { Remove-Item -LiteralPath $taskResolved -Recurse -Force }
}
