[CmdletBinding(SupportsShouldProcess = $true)]
param([Parameter(Mandatory = $true)][string]$GameDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'install_common.ps1')
$taskGameRoot = Get-NrRoot $GameDirectory
$taskLedgerPath = Get-NrChild $taskGameRoot '.open-nr-install.json'
$taskLedger = Get-Content -LiteralPath $taskLedgerPath -Raw | ConvertFrom-Json
if ($taskLedger.format -ne 'OpenNR-AMD-install-v1' -or
    -not $taskGameRoot.Equals([string]$taskLedger.gameDirectory, [StringComparison]::OrdinalIgnoreCase) -or
    $taskLedger.backupDirectory -notmatch '^\.open-nr-backup-[0-9a-f]{32}$') { throw 'Install ledger does not belong to this game directory' }
$taskBackupRoot = Get-NrChild $taskGameRoot $taskLedger.backupDirectory
$taskSeen = @{}
# Preflight every file before mutation. Modified game files are preserved and the
# user retains the ledger/backups to resolve them; uninstall never force-deletes.
foreach ($taskFile in $taskLedger.files) {
  $taskKey = $taskFile.destination.Replace('/', '\')
  if ($taskFile.installedSha256 -notmatch '^[0-9a-f]{64}$' -or $taskSeen.ContainsKey($taskKey) -or
      $taskFile.destination.StartsWith('.open-nr-', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid install ledger entry' }
  if ($taskFile.originalSha256 -and ($taskFile.originalSha256 -notmatch '^[0-9a-f]{64}$' -or $taskFile.backup -ne $taskFile.destination)) { throw 'Invalid backup entry' }
  $taskSeen[$taskKey] = $true
  $taskTarget = Get-NrChild $taskGameRoot $taskFile.destination
  $taskHash = if (Test-Path -LiteralPath $taskTarget) { Get-NrHash $taskTarget } else { $null }
  if ($taskHash -and $taskHash -ne $taskFile.installedSha256 -and $taskHash -ne $taskFile.originalSha256) {
    throw "Managed file changed after installation: $($taskFile.destination). It and the backups were preserved. Restore or move your changed file before retrying."
  }
  if ($taskFile.originalSha256 -and $taskHash -ne $taskFile.originalSha256) {
    $taskBackup = Get-NrChild $taskBackupRoot $taskFile.backup
    if ((Get-NrHash $taskBackup) -ne $taskFile.originalSha256) { throw 'Backup hash mismatch; refusing to change game files' }
  }
}
if (-not $PSCmdlet.ShouldProcess($taskGameRoot, 'Remove unchanged OpenNR files and restore verified originals')) { return }
$taskLedger.status = 'uninstalling'
Write-NrLedger $taskGameRoot $taskLedger
foreach ($taskFile in $taskLedger.files) {
  $taskTarget = Get-NrChild $taskGameRoot $taskFile.destination
  $taskHash = if (Test-Path -LiteralPath $taskTarget) { Get-NrHash $taskTarget } else { $null }
  if ($taskHash -and $taskHash -ne $taskFile.installedSha256 -and $taskHash -ne $taskFile.originalSha256) {
    throw 'Game file changed during uninstall; recovery ledger kept'
  }
  if ($taskFile.originalSha256) {
    if ($taskHash -ne $taskFile.originalSha256) {
      $taskBackup = Get-NrChild $taskBackupRoot $taskFile.backup
      if ((Get-NrHash $taskBackup) -ne $taskFile.originalSha256) { throw 'Backup changed during uninstall; recovery ledger kept' }
      Copy-Item -LiteralPath $taskBackup -Destination $taskTarget -Force
      if ((Get-NrHash $taskTarget) -ne $taskFile.originalSha256) { throw 'Restored original hash mismatch; recovery ledger kept' }
    }
  } elseif ($taskHash) {
    if ($taskHash -ne $taskFile.installedSha256) { throw 'Game file changed during uninstall; recovery ledger kept' }
    Remove-Item -LiteralPath $taskTarget
  }
}
# Only delete the individually verified backup files. Unlisted files in the
# backup directory, and the separately imported model, are never removed.
foreach ($taskFile in $taskLedger.files) {
  if ($taskFile.backup) {
    $taskBackup = Get-NrChild $taskBackupRoot $taskFile.backup
    if (Test-Path -LiteralPath $taskBackup) {
      if ((Get-NrHash $taskBackup) -eq $taskFile.originalSha256) { Remove-Item -LiteralPath $taskBackup }
    }
  }
}
Remove-Item -LiteralPath $taskLedgerPath
Write-Host "Uninstalled OpenNR AMD from $taskGameRoot. Imported model and unlisted files were preserved."
