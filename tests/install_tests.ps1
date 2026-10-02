$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts\install_common.ps1')
$taskScratch = Join-Path $taskRoot ('build\install-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskScratch | Out-Null
$taskChecks = 0
function Assert-Nr([bool]$Condition, [string]$Message) {
  ++$script:taskChecks
  if (-not $Condition) { throw $Message }
}
function Assert-NrThrows([scriptblock]$Action, [string]$Message) {
  ++$script:taskChecks
  $threw = $false
  try { & $Action } catch { $threw = $true }
  if (-not $threw) { throw $Message }
}
try {
  $taskPackage = Join-Path $taskScratch 'package'
  $taskGame = Join-Path $taskScratch 'game'
  New-Item -ItemType Directory -Path $taskPackage, $taskGame, (Join-Path $taskPackage 'payload'), (Join-Path $taskGame 'open-nr\model') | Out-Null
  $taskFiles = @()
  foreach ($taskSpec in @(@('OptiScaler.dll', 'proxy', 'proxy-new'), @('OpenNrRuntime.dll', 'runtime', 'runtime-new'), @('OptiScaler.ini', 'configuration', 'ini-new'))) {
    $taskSource = Join-Path $taskPackage ('payload\' + $taskSpec[0])
    Set-Content -LiteralPath $taskSource -Value $taskSpec[2] -NoNewline
    $taskFiles += [pscustomobject]@{ source = 'payload/' + $taskSpec[0]; destination = $taskSpec[0]; role = $taskSpec[1]; sha256 = Get-NrHash $taskSource }
  }
  $taskManifest = [pscustomobject]@{ format = 'OpenNR-AMD-package-v1'; files = $taskFiles }
  $taskManifestPath = Join-Path $taskPackage 'package-manifest.json'
  $taskManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskManifestPath
  Set-Content -LiteralPath (Join-Path $taskGame 'dxgi.dll') -Value 'original-proxy' -NoNewline
  Set-Content -LiteralPath (Join-Path $taskGame 'OptiScaler.ini') -Value 'original-ini' -NoNewline
  Set-Content -LiteralPath (Join-Path $taskGame 'unrelated.txt') -Value 'user-file' -NoNewline
  Set-Content -LiteralPath (Join-Path $taskGame 'open-nr\model\keep.bin') -Value 'user-model' -NoNewline
  & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame -WhatIf
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskGame '.open-nr-install.json'))) 'WhatIf wrote a ledger'
  & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskGame 'dxgi.dll') -Raw) -eq 'proxy-new') 'proxy was not installed'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskGame 'OpenNrRuntime.dll') -Raw) -eq 'runtime-new') 'runtime was not installed'
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame } 'reinstallation overwrote its ledger'
  & (Join-Path $taskRoot 'scripts\uninstall.ps1') -GameDirectory $taskGame -WhatIf
  Assert-Nr (Test-Path -LiteralPath (Join-Path $taskGame 'OpenNrRuntime.dll')) 'WhatIf removed a file'
  Set-Content -LiteralPath (Join-Path $taskGame 'OpenNrRuntime.dll') -Value 'user-edit' -NoNewline
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\uninstall.ps1') -GameDirectory $taskGame } 'modified managed file was removed'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskGame 'dxgi.dll') -Raw) -eq 'proxy-new') 'failed preflight partly restored game files'
  Copy-Item -LiteralPath (Join-Path $taskPackage 'payload\OpenNrRuntime.dll') -Destination (Join-Path $taskGame 'OpenNrRuntime.dll') -Force
  $taskLedger = Get-Content -LiteralPath (Join-Path $taskGame '.open-nr-install.json') -Raw | ConvertFrom-Json
  $taskBackup = Join-Path $taskGame ($taskLedger.backupDirectory + '\dxgi.dll')
  Set-Content -LiteralPath $taskBackup -Value 'tampered-original' -NoNewline
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\uninstall.ps1') -GameDirectory $taskGame } 'corrupted backup was restored'
  Set-Content -LiteralPath $taskBackup -Value 'original-proxy' -NoNewline
  & (Join-Path $taskRoot 'scripts\uninstall.ps1') -GameDirectory $taskGame
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskGame 'dxgi.dll') -Raw) -eq 'original-proxy') 'original proxy was not restored'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskGame 'OptiScaler.ini') -Raw) -eq 'original-ini') 'original configuration was not restored'
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskGame 'OpenNrRuntime.dll'))) 'new runtime was not removed'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskGame 'unrelated.txt') -Raw) -eq 'user-file') 'unrelated file changed'
  Assert-Nr ((Get-Content -LiteralPath (Join-Path $taskGame 'open-nr\model\keep.bin') -Raw) -eq 'user-model') 'separately imported model changed'
  Assert-Nr (-not (Test-Path -LiteralPath (Join-Path $taskGame '.open-nr-install.json'))) 'completed uninstall kept ledger'
  $taskFiles[1].destination = '../escape.dll'
  $taskManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskManifestPath
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame } 'package traversal path accepted'
  $taskFiles[1].destination = 'OpenNrRuntime.dll'
  $taskFiles[1].sha256 = '0' * 64
  $taskManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskManifestPath
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame } 'package hash mismatch accepted'
  $taskFiles[1].sha256 = Get-NrHash (Join-Path $taskPackage 'payload\OpenNrRuntime.dll')
  $taskFiles[1].destination = 'open-nr/model/stage0.bin'
  $taskManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskManifestPath
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame } 'packaged model weights accepted'
  $taskFiles[1].destination = 'CON.dll'
  $taskManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskManifestPath
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame } 'reserved Windows device path accepted'
  $taskFiles[1].destination = 'open-nr/shaders/test.spv'
  $taskFiles[2].destination = 'open-nr\shaders\test.spv'
  $taskManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskManifestPath
  Assert-NrThrows { & (Join-Path $taskRoot 'scripts\install.ps1') -PackageDirectory $taskPackage -GameDirectory $taskGame } 'equivalent duplicate destination paths accepted'
  Write-Host "Installer: $taskChecks synthetic checks passed; no actual game directory was used."
} finally {
  # Resolve and check this exact synthetic workspace path before recursive cleanup.
  $taskResolved = [IO.Path]::GetFullPath($taskScratch)
  $taskExpected = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')).TrimEnd('\') + '\install-tests-'
  if (-not $taskResolved.StartsWith($taskExpected, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing unsafe test cleanup path' }
  Remove-Item -LiteralPath $taskResolved -Recurse -Force
}
