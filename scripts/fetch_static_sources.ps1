[CmdletBinding(SupportsShouldProcess = $true)]
param()
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskSources = Join-Path $taskRoot 'integrations\optiscaler\sources'
$taskManifest = Get-Content -LiteralPath (Join-Path $taskSources 'manifest.json') -Raw | ConvertFrom-Json
if ($taskManifest.format -ne 'OpenNR-AMD-static-source-v1') { throw 'Unknown static-source manifest format' }
foreach ($taskArchive in $taskManifest.archives) {
  if ($taskArchive.file -notmatch '^[a-z0-9.-]+\.tar\.(?:gz|xz)$' -or $taskArchive.sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Invalid static-source archive identity' }
  $taskUri = [Uri]$taskArchive.url
  if ($taskUri.Scheme -ne 'https' -or $taskUri.Host -notin @('download.savannah.gnu.org','codeload.github.com')) { throw 'Unexpected static-source download origin' }
  $taskDestination = Join-Path $taskSources $taskArchive.file
  if (Test-Path -LiteralPath $taskDestination) {
    $taskExisting = Get-Item -LiteralPath $taskDestination
    if ($taskExisting.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Static-source archive is a reparse point' }
    if ($taskExisting.Length -ne $taskArchive.length -or (Get-FileHash -LiteralPath $taskDestination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskArchive.sha256) { throw "Existing archive differs from pin: $($taskArchive.file); refusing overwrite" }
    Write-Host "Verified $($taskArchive.file)"
    continue
  }
  if (-not $PSCmdlet.ShouldProcess($taskDestination, "Download hash-pinned source from $taskUri")) { continue }
  $taskTemporary = Join-Path $taskSources ('.static-source-' + [Guid]::NewGuid().ToString('N') + '.part')
  try {
    Invoke-WebRequest -Uri $taskUri -OutFile $taskTemporary
    if ((Get-Item -LiteralPath $taskTemporary).Length -ne $taskArchive.length -or (Get-FileHash -LiteralPath $taskTemporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskArchive.sha256) { throw "Downloaded archive differs from pin: $($taskArchive.file)" }
    if (Test-Path -LiteralPath $taskDestination) { throw 'Static-source destination appeared during download' }
    Move-Item -LiteralPath $taskTemporary -Destination $taskDestination
  } finally {
    if (Test-Path -LiteralPath $taskTemporary) { Remove-Item -LiteralPath $taskTemporary -Force }
  }
}
