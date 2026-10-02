# CPU-only shader resource analysis using the official portable RGA compiler.
# Example: ./scripts/analyze_amd_shaders.ps1 -FetchTool -Spirv build/shaders/amd_expert_ffn.spv `
#   -Source shaders/amd_expert_ffn.comp -Specialization @{0=256;10=16} `
#   -OutputDirectory build/performance/rga/expert256-k16 -DeclaredLdsBytes 22528
[CmdletBinding()]
param(
  [string]$Spirv,
  [string]$Source,
  [hashtable]$Specialization = @{},
  [string]$OutputDirectory,
  [ValidateSet('gfx1201')][string]$Target = 'gfx1201',
  [ValidateRange(0,65536)][uint32]$DeclaredLdsBytes = 0,
  [ValidateRange(1,65536)][uint32]$LdsBudgetBytes = 32768,
  [switch]$FetchTool,
  [switch]$LibraryOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-AmdFileSha256([string]$Path) {
  (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Convert-AmdSpirvSpecialization([byte[]]$Bytes, [hashtable]$Values) {
  if ($Bytes.Length -lt 20 -or $Bytes.Length % 4 -ne 0) { throw 'Malformed SPIR-V byte length' }
  [uint32[]]$words = [uint32[]]::new($Bytes.Length / 4)
  for ($i = 0; $i -lt $words.Length; ++$i) { $words[$i] = [BitConverter]::ToUInt32($Bytes, $i * 4) }
  if ($words[0] -ne 0x07230203 -or $words[4] -ne 0) { throw 'Malformed SPIR-V header' }
  $types = @{}; $decorations = @{}; $constants = @{}
  for ($i = 5; $i -lt $words.Length;) {
    $count = [int]($words[$i] -shr 16); $opcode = [int]($words[$i] -band 0xffff)
    if ($count -eq 0 -or [int64]$i + $count -gt $words.Length) { throw 'Malformed SPIR-V instruction bounds' }
    if ($opcode -eq 71 -and $count -ge 4 -and $words[$i+2] -eq 1) {
      $id = $words[$i+3].ToString()
      if ($decorations.ContainsKey($id)) { throw "Duplicate specialization ID $id" }
      $decorations[$id] = $words[$i+1]
    } elseif ($opcode -eq 20 -and $count -eq 2) {
      $types[$words[$i+1]] = 'bool'
    } elseif ($opcode -eq 21 -and $count -eq 4 -and $words[$i+2] -eq 32) {
      $types[$words[$i+1]] = if ($words[$i+3] -eq 0) { 'uint' } else { 'int' }
    } elseif ($opcode -eq 22 -and $count -eq 3 -and $words[$i+2] -eq 32) {
      $types[$words[$i+1]] = 'float'
    } elseif (($opcode -eq 48 -or $opcode -eq 49) -and $count -eq 3) {
      $constants[$words[$i+2]] = @{ position=$i; type=$words[$i+1]; opcode=$opcode }
    } elseif ($opcode -eq 50 -and $count -eq 4) {
      $constants[$words[$i+2]] = @{ position=$i; type=$words[$i+1]; opcode=$opcode }
    }
    $i += $count
  }
  $requested = @{}
  foreach ($key in $Values.Keys) {
    if ($key.ToString() -notmatch '^\d+$') { throw "Invalid specialization ID $key" }
    $id = ([uint32]$key).ToString()
    if ($requested.ContainsKey($id)) { throw "Duplicate requested specialization ID $id" }
    if (-not $decorations.ContainsKey($id)) { throw "Shader has no specialization ID $id" }
    $requested[$id] = $Values[$key]
  }
  $defaults = [ordered]@{}; $effective = [ordered]@{}
  foreach ($id in ($decorations.Keys | Sort-Object { [uint32]$_ })) {
    $result = $decorations[$id]
    if (-not $constants.ContainsKey($result)) {
      if ($requested.ContainsKey($id)) { throw "Specialization ID $id is not a scalar 32-bit constant" }
      continue
    }
    $entry = $constants[$result]; $position = $entry.position
    if (-not $types.ContainsKey($entry.type)) { throw "Specialization ID $id has an unsupported scalar type" }
    $kind = $types[$entry.type]
    if ($kind -eq 'bool') {
      if ($entry.opcode -ne 48 -and $entry.opcode -ne 49) { throw 'Malformed boolean specialization' }
      $default = $entry.opcode -eq 48
    } else {
      if ($entry.opcode -ne 50) { throw 'Malformed numeric specialization' }
      $literalBytes = [BitConverter]::GetBytes($words[$position+3])
      $default = switch ($kind) {
        'uint' { $words[$position+3] }
        'int' { [BitConverter]::ToInt32($literalBytes, 0) }
        'float' { [BitConverter]::ToSingle($literalBytes, 0) }
      }
    }
    $value = if ($requested.ContainsKey($id)) { $requested[$id] } else { $default }
    $defaults[$id] = @{type=$kind;value=$default}
    if ($kind -eq 'bool') {
      if ($value -isnot [bool] -and $value.ToString() -notin @('0','1')) { throw "Boolean specialization ID $id requires bool/0/1" }
      $normalized = if ($value -is [bool]) { $value } else { $value.ToString() -eq '1' }
      $words[$position] = [uint32]((3 -shl 16) -bor $(if ($normalized) { 48 } else { 49 }))
    } elseif ($kind -eq 'float') {
      $normalized = [single]$value
      if ([single]::IsNaN($normalized) -or [single]::IsInfinity($normalized)) { throw "Non-finite specialization ID $id" }
      $words[$position+3] = [BitConverter]::ToUInt32([BitConverter]::GetBytes($normalized), 0)
    } else {
      [decimal]$number = $value
      if ($number -ne [decimal]::Truncate($number)) { throw "Non-integral specialization ID $id" }
      if ($kind -eq 'uint') {
        if ($number -lt 0 -or $number -gt [uint32]::MaxValue) { throw "Out-of-range uint specialization ID $id" }
        $normalized = [uint32]$number; $words[$position+3] = $normalized
      } else {
        if ($number -lt [int32]::MinValue -or $number -gt [int32]::MaxValue) { throw "Out-of-range int specialization ID $id" }
        $normalized = [int32]$number
        $words[$position+3] = [BitConverter]::ToUInt32([BitConverter]::GetBytes($normalized), 0)
      }
    }
    $effective[$id] = @{type=$kind;value=$normalized}
  }
  [byte[]]$outputBytes = [byte[]]::new($Bytes.Length)
  for ($i=0; $i -lt $words.Length; ++$i) { [BitConverter]::GetBytes($words[$i]).CopyTo($outputBytes, $i*4) }
  return @{ bytes=$outputBytes; defaults=$defaults; effective=$effective; generator=$words[2]; version=$words[1] }
}

function Get-AmdSourceClosure([string]$Path) {
  $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  $records = [System.Collections.Generic.List[object]]::new()
  function Visit-AmdInclude([string]$File) {
    $filePath = (Resolve-Path -LiteralPath $File).Path
    if (-not $seen.Add($filePath)) { return }
    $records.Add(@{file=$filePath;sha256=(Get-AmdFileSha256 $filePath)})
    $text = Get-Content -LiteralPath $filePath -Raw
    foreach ($include in [regex]::Matches($text, '(?m)^\s*#\s*include\s+"([^"]+)"')) {
      Visit-AmdInclude (Join-Path (Split-Path -Parent $filePath) $include.Groups[1].Value)
    }
  }
  Visit-AmdInclude $Path
  return $records.ToArray()
}

function Invoke-AmdOfflineTool([string]$Executable, [string[]]$Arguments, [string]$Log) {
  $text = (& $Executable @Arguments 2>&1 | Out-String)
  $exit = $LASTEXITCODE
  [IO.File]::WriteAllText($Log, $text)
  if ($exit -ne 0) { throw "Offline compiler/tool failed ($exit); see $Log" }
  return $text
}

function Get-AmdMetadataNumber([string]$Text, [string]$Field) {
  $matches = [regex]::Matches($Text, '(?m)^\s*\.' + [regex]::Escape($Field) + ':\s*(\d+)\s*$')
  if ($matches.Count -ne 1) { throw "Missing or ambiguous ELF metadata field $Field" }
  return [uint64]$matches[0].Groups[1].Value
}

if ($LibraryOnly) { return }
$repo = Split-Path -Parent $PSScriptRoot
$identityPath = Join-Path $PSScriptRoot 'rga_tool_manifest.json'
$identity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
$toolRoot = Join-Path $repo 'tools/rga'
$archive = Join-Path $toolRoot $identity.archiveFile
$install = Join-Path $toolRoot $identity.releaseTag
if ($FetchTool) {
  New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
  if (-not (Test-Path -LiteralPath $archive)) {
    $download = $archive + '.' + [guid]::NewGuid().ToString('N') + '.partial'
    Invoke-WebRequest -Uri $identity.archiveUrl -OutFile $download
    if ((Get-Item -LiteralPath $download).Length -ne $identity.archiveBytes -or
        (Get-AmdFileSha256 $download) -ne $identity.archiveSha256) { throw 'RGA download identity mismatch; partial file retained' }
    Move-Item -LiteralPath $download -Destination $archive
  }
  if ((Get-Item -LiteralPath $archive).Length -ne $identity.archiveBytes -or
      (Get-AmdFileSha256 $archive) -ne $identity.archiveSha256) { throw 'RGA archive identity mismatch' }
  if (-not (Test-Path -LiteralPath $install)) {
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
      $prefix = [IO.Path]::GetFullPath($install) + [IO.Path]::DirectorySeparatorChar
      foreach ($entry in $zip.Entries) {
        $entryPath = [IO.Path]::GetFullPath((Join-Path $install $entry.FullName))
        if (-not $entryPath.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'RGA archive path escapes its install directory' }
      }
    } finally { $zip.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $install)
  }
}
foreach ($file in $identity.files.PSObject.Properties) {
  $path = Join-Path $install $file.Name
  if (-not (Test-Path -LiteralPath $path) -or (Get-AmdFileSha256 $path) -ne $file.Value) {
    throw "RGA tool identity mismatch/missing: $($file.Name); use -FetchTool for a fresh pinned installation"
  }
}
foreach ($notice in $identity.noticeFiles) {
  if (-not (Test-Path -LiteralPath (Join-Path $install $notice))) { throw "Missing portable tool notice $notice" }
}
if (-not $Spirv) {
  if ($FetchTool) { Write-Host "Pinned RGA $($identity.releaseTag) portable tool verified at $install"; return }
  throw 'Specify -Spirv and a fresh -OutputDirectory, or use -FetchTool only to download the portable tool'
}
if (-not $OutputDirectory) { throw 'A fresh -OutputDirectory is required' }
$inputPath = (Resolve-Path -LiteralPath $Spirv).Path
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputPath) { throw 'Output directory already exists; refusing to overwrite analysis evidence' }
$specialized = Convert-AmdSpirvSpecialization ([IO.File]::ReadAllBytes($inputPath)) $Specialization
$sources = if ($Source) { @(Get-AmdSourceClosure $Source) } else { @() }
New-Item -ItemType Directory -Path $outputPath | Out-Null
$privateSpirv = Join-Path $outputPath 'specialized.spv'
[IO.File]::WriteAllBytes($privateSpirv, $specialized.bytes)
$elf = Join-Path $outputPath 'wave32.elf'
$llpc = Join-Path $install 'utils/amdllpc.exe'
$compilerArguments = @('--gfxip=12.0.1','--subgroup-size=32','--native-wave-size=32',
  '--auto-layout-desc','--include-llvm-ir',('-o=' + $elf.Replace('\','/')),$privateSpirv.Replace('\','/'))
$null = Invoke-AmdOfflineTool $llpc $compilerArguments (Join-Path $outputPath 'compiler.log')
if (-not (Test-Path -LiteralPath $elf) -or (Get-Item -LiteralPath $elf).Length -eq 0) { throw 'Offline compiler did not create a nonempty ELF' }
$metadata = Invoke-AmdOfflineTool (Join-Path $install 'utils/lc/opencl/bin/llvm-readobj.exe') @('--notes',$elf) (Join-Path $outputPath 'elf-metadata.txt')
$wave = Get-AmdMetadataNumber $metadata 'wavefront_size'
if ($wave -ne 32) { throw "Offline compiler emitted wave$wave instead of required wave32" }
$isaPath = Join-Path $outputPath 'wave32.isa'
$null = Invoke-AmdOfflineTool (Join-Path $install 'utils/lc/disassembler/amdgpu-dis.exe') @($elf,'-o',$isaPath) (Join-Path $outputPath 'disassembler.log')
$isa = Get-Content -LiteralPath $isaPath -Raw
$lds = Get-AmdMetadataNumber $metadata 'lds_size'
$rgaArguments = @('-s','bin','--co',$elf,'-a',(Join-Path $outputPath 'rga-stats.csv'),
  '--isa',(Join-Path $outputPath 'rga-binary.isa'))
$null = Invoke-AmdOfflineTool (Join-Path $install 'rga.exe') $rgaArguments (Join-Path $outputPath 'rga-binary-analysis.log')
$csvFiles = @(Get-ChildItem -LiteralPath $outputPath -Filter ($Target + '_rga-stats*.csv'))
if ($csvFiles.Count -ne 1) { throw 'RGA binary analysis did not produce one compute statistics CSV' }
$csv = @(Import-Csv -LiteralPath $csvFiles[0].FullName)
if ($csv.Count -ne 1 -or $csv[0].DEVICE -ne $Target) { throw 'RGA binary analysis target/row mismatch' }
$statistics = $csv[0]
foreach ($pair in @(@('USED_LDS_BYTES','lds_size'),@('USED_SGPRs','sgpr_count'),
                    @('USED_VGPRs','vgpr_count'),@('SCRATCH_MEM','scratch_memory_size'))) {
  if ([uint64]$statistics.($pair[0]) -ne (Get-AmdMetadataNumber $metadata $pair[1])) {
    throw "RGA binary statistics disagree with compiled ELF metadata: $($pair[0])"
  }
}
$dimensionsMatch = [regex]::Match($metadata,'(?m)^\s*\.threadgroup_dimensions:\s*\r?\n\s*- (\d+)\s*\r?\n\s*- (\d+)\s*\r?\n\s*- (\d+)')
if (-not $dimensionsMatch.Success) { throw 'Missing compiled threadgroup dimensions' }
$threadgroup = @(1..3 | ForEach-Object { [uint32]$dimensionsMatch.Groups[$_].Value })
$matrixInstructions = @([regex]::Matches($isa,'(?m)^\s*(v_(?:wmma|mfma)_\S+)') | ForEach-Object { $_.Groups[1].Value } | Group-Object | ForEach-Object { @{instruction=$_.Name;staticCount=$_.Count} })
$report = [ordered]@{
  schemaVersion=1; mode='portable-rga-bundled-llpc-offline'; target=$Target; installedDriverEquivalent=$false; gpuExecuted=$false
  limitations='Auto-reflected descriptors and offline LLPC differ from the installed driver pipeline. A runtime cpso with specialization and required subgroup state plus installed-driver statistics remains necessary for final delivery evidence. Static instruction counts are not runtime execution counts. Spill counts are null when not independently reported.'
  toolIdentity=$identity; toolIdentityManifestSha256=(Get-AmdFileSha256 $identityPath)
  analysisScriptSha256=(Get-AmdFileSha256 $PSCommandPath)
  compilerInvocation=@{executable='utils/amdllpc.exe';arguments=$compilerArguments;requiredSubgroupSize=32;nativeWaveSize=32}
  binaryAnalysisInvocation=@{executable='rga.exe';arguments=$rgaArguments;statisticsSha256=(Get-AmdFileSha256 $csvFiles[0].FullName)}
  input=@{file=$inputPath;sha256=(Get-AmdFileSha256 $inputPath);generator=$specialized.generator;spirvVersion=$specialized.version}
  sourceFiles=$sources; sourceCorrespondence='Caller-supplied source closure hashes; this analysis does not prove how the input SPIR-V was built'
  defaultSpecialization=$specialized.defaults; effectiveSpecialization=$specialized.effective
  specializedSpirvSha256=(Get-AmdFileSha256 $privateSpirv); elfSha256=(Get-AmdFileSha256 $elf); isaSha256=(Get-AmdFileSha256 $isaPath)
  resources=@{wavefrontSize=$wave;threadgroupDimensions=$threadgroup;ldsBytes=$lds;sgprs=(Get-AmdMetadataNumber $metadata 'sgpr_count');vgprs=(Get-AmdMetadataNumber $metadata 'vgpr_count');scratchBytes=(Get-AmdMetadataNumber $metadata 'scratch_memory_size');sgprSpills=[uint64]$statistics.SGPR_SPILLS;vgprSpills=[uint64]$statistics.VGPR_SPILLS;staticScratchInstructionCount=[regex]::Matches($isa,'(?m)^\s*(?:scratch_|buffer_\S*scratch)\S*').Count}
  matrixInstructions=$matrixInstructions; declaredLdsBytes=$(if ($DeclaredLdsBytes) { $DeclaredLdsBytes } else { $null }); compilerAddedLdsBytes=$(if ($DeclaredLdsBytes) { [int64]$lds - [int64]$DeclaredLdsBytes } else { $null }); ldsBudgetBytes=$LdsBudgetBytes; fitsLdsBudget=($lds -le $LdsBudgetBytes)
}
$report | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $outputPath 'analysis.json') -Encoding utf8
Write-Host "Offline wave32: LDS=$lds bytes, SGPR=$($report.resources.sgprs), VGPR=$($report.resources.vgprs), scratch=$($report.resources.scratchBytes) bytes; $outputPath"
if ($DeclaredLdsBytes -and $lds -gt $DeclaredLdsBytes) { Write-Warning 'Compiler adds LDS beyond source declarations; use compiled LDS for the qualification budget (see analysis.json)' }
if (-not $report.fitsLdsBudget) { throw 'Compiled LDS exceeds qualification budget; inspect analysis.json' }
