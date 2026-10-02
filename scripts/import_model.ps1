param(
  [Parameter(Mandatory = $true)][string]$NvidiaDll,
  [Parameter(Mandatory = $true)][string]$Destination,
  [string]$Importer
)
$ErrorActionPreference = 'Stop'
if (-not $Importer) {
  $taskRoot = Split-Path -Parent $PSScriptRoot
  $taskBuiltImporter = Join-Path $taskRoot 'build\importer\model_importer.exe'
  $taskPackagedImporter = Join-Path $taskRoot 'tools\model_importer.exe'
  $Importer = if (Test-Path -LiteralPath $taskBuiltImporter -PathType Leaf) { $taskBuiltImporter } else { $taskPackagedImporter }
}
if (-not (Test-Path -LiteralPath $Importer -PathType Leaf)) {
  throw 'Build the importer first with scripts/build_importer.ps1, or pass -Importer from the package.'
}
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists. Choose a new model directory.' }
& $Importer ([IO.Path]::GetFullPath($NvidiaDll)) ([IO.Path]::GetFullPath($Destination))
if ($LASTEXITCODE -ne 0) { throw 'Model import failed; no completed model was published.' }
