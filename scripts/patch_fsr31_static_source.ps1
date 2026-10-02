param([Parameter(Mandatory = $true)][string]$SourceDirectory)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
if (-not (Test-Path -LiteralPath $taskRoot -PathType Container)) { throw 'Extracted FSR31 source directory is missing' }
$taskPins = @(
  [pscustomobject]@{ path = 'sdk\include\FidelityFX\host\backends\dx11\ffx_dx11.h'; original = 'c347457ae82d40aff5234e4ddef7c881ee72c7d4289e9da0ee469a5b470d13ab'; patched = '95e1cd36ff33818ee0d096ed1c31db2011bf1d3eb751571c78a3254d0b13a2fe' },
  [pscustomobject]@{ path = 'sdk\src\backends\dx11\ffx_dx11.cpp'; original = '42628dda209eeb99a74d50f41b3eca41e0a861b4f564ddcef8fd91f7a2dc1010'; patched = '6f70629c5b6a30a17059c8949eb271ccf9e145202787f33db18dd0da5cc39128' }
)
$taskPrepared = @()
foreach ($taskPin in $taskPins) {
  $taskPath = [IO.Path]::GetFullPath((Join-Path $taskRoot $taskPin.path))
  if (-not $taskPath.StartsWith($taskRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Source patch escapes source root' }
  $taskAncestor = $taskPath
  while ($taskAncestor) {
    if ((Get-Item -LiteralPath $taskAncestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source patch path contains a reparse point' }
    $taskAncestor = [IO.Path]::GetDirectoryName($taskAncestor)
  }
  $taskHash = (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($taskHash -eq $taskPin.patched) { continue }
  if ($taskHash -ne $taskPin.original) { throw "Source differs from pinned FSR31 version: $($taskPin.path); no files changed" }
  $taskText = [IO.File]::ReadAllText($taskPath)
  $taskText = $taskText.Replace('ffxGetDeviceDX11','ffxGetDeviceDX11_Fsr31').Replace('ffxGetResourceDX11','ffxGetResourceDX11_Fsr31')
  $taskPrepared += [pscustomobject]@{path=$taskPath;text=$taskText;expected=$taskPin.patched}
}
# Validate every input before modifying either file. Already-patched files make
# reruns safe; no library is built or replaced by this source-only operation.
foreach ($taskFile in $taskPrepared) {
  [IO.File]::WriteAllText($taskFile.path, $taskFile.text, [Text.UTF8Encoding]::new($false))
  if ((Get-FileHash -LiteralPath $taskFile.path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskFile.expected) { throw 'Patched source hash mismatch' }
}
Write-Host 'FSR31 DX11 device/resource C symbols match the host header names; libraries were not rebuilt.'
