# Validate a locally imported model without running NVIDIA's DLL. Exported
# activations are local diagnostics and are deliberately excluded from packages.
param(
  [string]$Model = '',
  [int]$Width = 320,
  [int]$Height = 320,
  [int]$Frames = 3,
  [switch]$SkipBuild,
  [switch]$SkipWebGpu,
  [switch]$OptimizedWebGpu,
  [string]$Browser = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Model) { $Model = Join-Path $root 'models\imported\open-nr' }
$Model = (Resolve-Path -LiteralPath $Model).Path
if ($Width -lt 320 -or $Height -lt 320 -or $Frames -lt 1) { throw 'Width/Height must be >=320 and Frames >=1.' }
if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot 'build.ps1') -Backend amd
  if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
}
$executable = Join-Path $root 'build\dlss5vk.exe'
$fixtureRoot = Join-Path $root 'build\model-validation'
$referenceName = "reference-$Width-$Height"
$amdName = "amd-$Width-$Height"
$reference = Join-Path $fixtureRoot $referenceName
$amd = Join-Path $fixtureRoot $amdName
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null
& $executable modelcheck --backend reference --model $Model --fixture $reference --width $Width --height $Height --frames $Frames 2>&1 |
  Tee-Object -FilePath (Join-Path $fixtureRoot "$referenceName.log")
if ($LASTEXITCODE -ne 0) { throw 'Software-reference validation failed.' }
& $executable modelcheck --backend amd --model $Model --fixture $amd --reference $reference --width $Width --height $Height --frames $Frames 2>&1 |
  Tee-Object -FilePath (Join-Path $fixtureRoot "$amdName.log")
if ($LASTEXITCODE -ne 0) { throw 'AMD validation failed to execute.' }
if (-not $SkipWebGpu) {
  if (-not $Browser) {
    $Browser = @(
      'C:\Program Files\Google\Chrome\Application\chrome.exe',
      'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe',
      'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
  }
  if (-not $Browser -or -not (Test-Path -LiteralPath $Browser)) { throw 'No Chromium browser found. Supply -Browser or explicitly use -SkipWebGpu.' }
  $previousWeights = $env:NR_WEIGHTS
  $previousFixtures = $env:NR_FIXTURES
  $previousChrome = $env:CHROME
  try {
    $env:NR_WEIGHTS = $Model
    $env:NR_FIXTURES = $fixtureRoot
    $env:CHROME = $Browser
    $webgpuPage = if ($OptimizedWebGpu) { 'parity' } else { 'direct' }
    & node (Join-Path $root 'ports\browser-webgpu\tools\headless.mjs') $webgpuPage "repeat=1&fixture=/fixtures/$referenceName" 2>&1 |
      Tee-Object -FilePath (Join-Path $fixtureRoot "$referenceName-webgpu-$webgpuPage.log")
    if ($LASTEXITCODE -ne 0) { throw 'Independent WebGPU/reference parity failed.' }
  } finally {
    $env:NR_WEIGHTS = $previousWeights
    $env:NR_FIXTURES = $previousFixtures
    $env:CHROME = $previousChrome
  }
}
Write-Host "Local diagnostics: $fixtureRoot"
Write-Host 'AMD differences are reported without an automatic visual-quality acceptance threshold. These tests do not establish NVIDIA-runtime parity.'
