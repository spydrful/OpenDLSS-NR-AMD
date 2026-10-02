[CmdletBinding(SupportsShouldProcess = $true)]
param(
  [Parameter(Mandatory = $true)][string]$PackageDirectory,
  [Parameter(Mandatory = $true)][string]$GameDirectory,
  [ValidateSet('dxgi.dll', 'winmm.dll', 'version.dll')][string]$ProxyName = 'dxgi.dll'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'install_common.ps1')
$taskPackageRoot = Get-NrRoot $PackageDirectory
$taskGameRoot = Get-NrRoot $GameDirectory
$taskPackage = Read-NrPackage $taskPackageRoot
$taskLedgerPath = Get-NrChild $taskGameRoot '.open-nr-install.json'
if (Test-Path -LiteralPath $taskLedgerPath) { throw 'An OpenNR install ledger already exists. Uninstall it before installing again.' }
$taskBackupName = '.open-nr-backup-' + [Guid]::NewGuid().ToString('N')
$taskBackupRoot = Get-NrChild $taskGameRoot $taskBackupName
$taskFiles = @()
$taskTargets = @{}
foreach ($taskEntry in $taskPackage.files) {
  $taskRelative = if ($taskEntry.role -eq 'proxy') { $ProxyName } else { [string]$taskEntry.destination }
  $taskKey = $taskRelative.Replace('/', '\')
  if ($taskRelative.StartsWith('.open-nr-', [StringComparison]::OrdinalIgnoreCase) -or $taskTargets.ContainsKey($taskKey)) {
    throw 'A package target conflicts with installation metadata or another file'
  }
  $taskTargets[$taskKey] = $true
  $taskTarget = Get-NrChild $taskGameRoot $taskRelative
  $taskOriginalHash = $null
  if (Test-Path -LiteralPath $taskTarget) { $taskOriginalHash = Get-NrHash $taskTarget }
  $taskFiles += [pscustomobject]@{
    source = [string]$taskEntry.source; destination = $taskRelative;
    installedSha256 = $taskEntry.sha256.ToLowerInvariant(); originalSha256 = $taskOriginalHash;
    backup = if ($taskOriginalHash) { $taskRelative } else { $null }; phase = 'planned'
  }
}
if (-not $PSCmdlet.ShouldProcess($taskGameRoot, "Install OpenNR AMD and back up $($taskFiles.Count) managed targets")) { return }
New-Item -ItemType Directory -Path $taskBackupRoot | Out-Null
$taskLedger = [pscustomobject]@{
  format = 'OpenNR-AMD-install-v1'; gameDirectory = $taskGameRoot; backupDirectory = $taskBackupName;
  installedUtc = [DateTime]::UtcNow.ToString('o'); status = 'installing'; files = $taskFiles
}
# Write recovery information before replacing any game file. Each backup is made
# before its target is touched; the ledger survives an interrupted installation.
Write-NrLedger $taskGameRoot $taskLedger
try {
  foreach ($taskFile in $taskLedger.files) {
    $taskTarget = Get-NrChild $taskGameRoot $taskFile.destination
    $taskSource = Get-NrChild $taskPackageRoot $taskFile.source
    if ((Get-NrHash $taskSource) -ne $taskFile.installedSha256) { throw 'Package changed during installation' }
    if ($taskFile.originalSha256) {
      if ((Get-NrHash $taskTarget) -ne $taskFile.originalSha256) { throw 'Game file changed during installation' }
      $taskBackup = Get-NrChild $taskBackupRoot $taskFile.backup
      New-Item -ItemType Directory -Path (Split-Path -Parent $taskBackup) -Force | Out-Null
      Copy-Item -LiteralPath $taskTarget -Destination $taskBackup
      if ((Get-NrHash $taskBackup) -ne $taskFile.originalSha256) { throw 'Backup verification failed' }
    } elseif (Test-Path -LiteralPath $taskTarget) { throw 'A new game file appeared during installation' }
    $taskFile.phase = 'backed-up'
    Write-NrLedger $taskGameRoot $taskLedger
    New-Item -ItemType Directory -Path (Split-Path -Parent $taskTarget) -Force | Out-Null
    Copy-Item -LiteralPath $taskSource -Destination $taskTarget -Force
    if ((Get-NrHash $taskTarget) -ne $taskFile.installedSha256) { throw 'Installed file verification failed' }
    $taskFile.phase = 'installed'
    Write-NrLedger $taskGameRoot $taskLedger
  }
  $taskLedger.status = 'installed'
  Write-NrLedger $taskGameRoot $taskLedger
} catch {
  $taskLedger.status = 'interrupted'
  Write-NrLedger $taskGameRoot $taskLedger
  throw "Installation stopped. Recovery ledger and existing backups were preserved; run uninstall.ps1 for this game directory. $($_.Exception.Message)"
}
Write-Host "Installed OpenNR AMD in $taskGameRoot. Import your locally supplied model separately into open-nr\model."
