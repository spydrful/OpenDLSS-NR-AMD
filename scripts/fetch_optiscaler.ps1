$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$hostRoot = Join-Path $root 'third_party\optiscaler-host'
$commit = '557bb8553098395f5f138c2e22ed25f256f7a3a2'
if (-not (Test-Path -LiteralPath $hostRoot)) {
  git clone --no-checkout https://github.com/MatheusFerreiraS/neural-amd-opti.git $hostRoot
  if ($LASTEXITCODE -ne 0) { throw 'OptiScaler clone failed' }
  git -c "safe.directory=$($hostRoot.Replace('\','/'))" -C $hostRoot checkout --detach $commit
  if ($LASTEXITCODE -ne 0) { throw 'OptiScaler pinned checkout failed' }
}
$actual = git -c "safe.directory=$($hostRoot.Replace('\','/'))" -C $hostRoot rev-parse HEAD
if ($actual -ne $commit) { throw "OptiScaler checkout must be at $commit; refusing to replace local work." }
git -c "safe.directory=$($hostRoot.Replace('\','/'))" -C $hostRoot submodule update --init external/simpleini external/unordered_dense external/xess external/vulkan external/spdlog external/FidelityFX-SDK external/magic_enum external/FidelityFX-SDK-v2 external/nvapi
if ($LASTEXITCODE -ne 0) { throw 'Pinned dependency fetch failed' }
& "$PSScriptRoot\patch_optiscaler.ps1"
