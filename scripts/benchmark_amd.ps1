[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ModelDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dlss5vk.exe'),
    [string]$ShaderDirectory,
    [string]$Python = 'python',
    [ValidateSet('bench', 'profile')][string]$Mode = 'bench',
    [ValidateSet('auto', 'baseline', 'optimized')][string]$Kernels = 'optimized',
    [ValidateSet('k16', 'k32', 'final')][string]$Arithmetic = 'k16',
    [ValidateSet(16, 32, 64)][int]$TileN = 16,
    [ValidateSet(16, 32, 64)][int]$StageK = 16,
    [ValidateSet(16,32,64)][int]$WindowQueries = 64,
    [ValidateSet('legacy', 'compact64')][string]$ComparisonAnchor = 'legacy',
    [ValidateRange(1, 32768)][int]$Width = 1707,
    [ValidateRange(1, 32768)][int]$Height = 960,
    [ValidateRange(0, 10000)][int]$Warmup = 5,
    [ValidateRange(1, 100000)][int]$Frames = 30,
    [ValidateRange(1, 100)][int]$Pairs = 3,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 900,
    [switch]$AllowArithmeticChange,
    [switch]$Fusion,
    [switch]$ExpertFusion,
    [switch]$BlockFusion,
    [switch]$HardwarePublication
)
$ErrorActionPreference = 'Stop'
$taskTool = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\tune_amd.py'
$taskArguments = @(
    $taskTool, 'collect', '--executable', $Executable, '--model', $ModelDirectory,
    '--output', $OutputDirectory, '--mode', $Mode, '--kernels', $Kernels,
    '--arithmetic', $Arithmetic, '--tile-n', "$TileN", '--stage-k', "$StageK",
    '--window-queries',"$WindowQueries",
    '--comparison-anchor', $ComparisonAnchor,
    '--width', "$Width", '--height', "$Height", '--warmup', "$Warmup",
    '--frames', "$Frames", '--pairs', "$Pairs", '--timeout', "$TimeoutSeconds"
)
if ($ShaderDirectory) { $taskArguments += @('--shaders', $ShaderDirectory) }
if ($AllowArithmeticChange) { $taskArguments += '--allow-arithmetic-change' }
if ($Fusion) { $taskArguments += '--fusion' }
if ($ExpertFusion) { $taskArguments += '--expert-fusion' }
if ($BlockFusion) { $taskArguments += '--block-fusion' }
if ($HardwarePublication) { $taskArguments += '--hardware-publication' }
# The Python driver sets child-only environment values; caller state is retained.
& $Python @taskArguments
if ($LASTEXITCODE -ne 0) { throw "AMD benchmark driver failed with exit $LASTEXITCODE" }
