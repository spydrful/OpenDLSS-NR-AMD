# Shared package/ledger validation. No shell-built file operation commands.
Set-StrictMode -Version Latest
function Get-NrRoot([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
  if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "Directory does not exist: $full" }
  if ((Get-Item -LiteralPath $full -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw "A reparse-point directory is not supported: $full"
  }
  return $full
}
function Get-NrChild([string]$Root, [string]$Relative) {
  if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or $Relative.Contains(':')) {
    throw "Invalid package/ledger relative path: $Relative"
  }
  $parts = $Relative -split '[\\/]'
  foreach ($part in $parts) {
    if ([string]::IsNullOrWhiteSpace($part) -or $part -eq '.' -or $part -eq '..' -or
        $part -match '[\x00-\x1f<>"|?*]' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
        $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
      throw "Unsafe package/ledger relative path: $Relative"
    }
  }
  $full = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
  $prefix = $Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes the intended directory' }
  $probe = $Root
  foreach ($part in $parts) {
    $probe = Join-Path $probe $part
    if (Test-Path -LiteralPath $probe) {
      if ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "A reparse point blocks this operation: $probe"
      }
    }
  }
  return $full
}
function Get-NrHash([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Expected a regular file: $Path" }
  if ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point file: $Path" }
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Write-NrLedger([string]$Root, $Ledger) {
  $target = Get-NrChild $Root '.open-nr-install.json'
  $temporary = Get-NrChild $Root '.open-nr-install.json.tmp'
  $Ledger | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8
  Move-Item -LiteralPath $temporary -Destination $target -Force
}
function Read-NrPackage([string]$Root) {
  $manifestPath = Get-NrChild $Root 'package-manifest.json'
  $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  if ($manifest.format -ne 'OpenNR-AMD-package-v1' -or @($manifest.files).Count -eq 0) { throw 'Unsupported or empty package manifest' }
  $destinations = @{}
  $sources = @{}
  $proxyCount = 0
  foreach ($entry in $manifest.files) {
    if ($entry.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid package hash' }
    $source = Get-NrChild $Root $entry.source
    if ((Get-NrHash $source) -ne $entry.sha256.ToLowerInvariant()) { throw "Package hash mismatch: $($entry.source)" }
    $null = Get-NrChild $Root $entry.destination
    if ($entry.destination -match '(^|[\\/])model([\\/]|$)' -or $entry.source -match '(?i)nvngx|dlssnr\.bin|stage\d+\.bin|weights') {
      throw 'A package must not contain NVIDIA binaries or model weights'
    }
    if ($entry.role -eq 'proxy') { ++$proxyCount }
    $destinationKey = $entry.destination.Replace('/', '\')
    $sourceKey = $entry.source.Replace('/', '\')
    if ($destinations.ContainsKey($destinationKey) -or $sources.ContainsKey($sourceKey)) { throw 'Duplicate package file' }
    $destinations[$destinationKey] = $true
    $sources[$sourceKey] = $true
  }
  if ($proxyCount -ne 1) { throw 'The package must contain exactly one OptiScaler proxy DLL' }
  return $manifest
}
