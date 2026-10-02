[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\presentmon'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskUrl = 'https://github.com/GameTechDev/PresentMon/releases/download/v2.3.1/PresentMon-2.3.1-x64.exe'
$taskHash = '364e5d98d4d134bd54dd25c22ed2ca2f4883f8bc3ed6502bee0c151e3436d30c'
$taskDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$taskAncestor = $taskDirectory
while ($taskAncestor) {
  if (Test-Path -LiteralPath $taskAncestor) {
    $taskItem = Get-Item -LiteralPath $taskAncestor -Force
    if ($taskItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'PresentMon destination must not traverse reparse points' }
  }
  $taskAncestor = [IO.Path]::GetDirectoryName($taskAncestor)
}
$null = [IO.Directory]::CreateDirectory($taskDirectory)
$taskTarget = Join-Path $taskDirectory 'PresentMon-2.3.1-x64.exe'
if (Test-Path -LiteralPath $taskTarget) {
  if ((Get-FileHash -LiteralPath $taskTarget -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskHash) { throw 'Existing PresentMon differs from the pinned binary; refusing replacement' }
  Write-Output $taskTarget
  return
}
$taskDownload = Join-Path $taskDirectory ('.presentmon-' + [Guid]::NewGuid().ToString('N') + '.download')
try {
  Invoke-WebRequest -Uri $taskUrl -OutFile $taskDownload
  if ((Get-FileHash -LiteralPath $taskDownload -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskHash) { throw 'PresentMon download SHA-256 differs from the pinned release' }
  [IO.File]::Move($taskDownload, $taskTarget) # No replacement if another download won the race.
  Write-Output $taskTarget
} finally {
  if (Test-Path -LiteralPath $taskDownload) { Remove-Item -LiteralPath $taskDownload }
}
