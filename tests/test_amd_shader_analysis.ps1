# Synthetic CPU parser checks; never invokes RGA, Vulkan or a GPU.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/analyze_amd_shaders.ps1') -LibraryOnly
$checks = 0
function Check-Analysis([bool]$Condition, [string]$Name) {
  if (-not $Condition) { throw "Analysis parser test failed: $Name" }
  $script:checks++
}
function Expect-AnalysisReject([scriptblock]$Body, [string]$Name) {
  $rejected = $false
  try { & $Body | Out-Null } catch { $rejected = $true }
  Check-Analysis $rejected $Name
}
function Words-ToBytes([uint32[]]$Words) {
  $bytes = [byte[]]::new($Words.Length * 4)
  for ($i=0; $i -lt $Words.Length; ++$i) { [BitConverter]::GetBytes($Words[$i]).CopyTo($bytes,$i*4) }
  return ,$bytes
}
# Minimal synthetic instruction stream: scalar uint/bool/float/int specs plus
# an opaque unknown capability. Float8 modules remain byte-preserved without
# depending on the older SPIR-V disassembler shipped by RGA.
[uint32[]]$fixture = @(
  0x07230203, 0x00010600, 123, 12, 0,
  0x00020011, 4212,
  0x00040047, 2, 1, 10,
  0x00040047, 4, 1, 13,
  0x00040047, 6, 1, 15,
  0x00040047, 8, 1, 16,
  0x00040015, 1, 32, 0,
  0x00020014, 3,
  0x00030016, 5, 32,
  0x00040015, 7, 32, 1,
  0x00040032, 1, 2, 16,
  0x00030031, 3, 4,
  0x00040032, 5, 6, 0x3f800000,
  0x00040032, 7, 8, 0
)
$bytes = Words-ToBytes $fixture
$untouched = Convert-AmdSpirvSpecialization $bytes @{}
Check-Analysis ([Convert]::ToBase64String($untouched.bytes) -eq [Convert]::ToBase64String($bytes)) 'identity preserves every byte'
Check-Analysis ($untouched.effective['10'].value -eq 16) 'uint default'
Check-Analysis ($untouched.effective['13'].value -is [bool] -and -not $untouched.effective['13'].value) 'bool default type'
$changed = Convert-AmdSpirvSpecialization $bytes @{10=32;13=$true;15=1.5;16=-17}
Check-Analysis ($changed.effective['10'].value -eq 32) 'uint specialization'
Check-Analysis ($changed.effective['13'].value -is [bool] -and $changed.effective['13'].value) 'bool specialization type'
Check-Analysis ($changed.effective['15'].value -eq [single]1.5) 'float specialization'
Check-Analysis ($changed.effective['16'].value -eq -17) 'signed specialization'
$roundtrip = Convert-AmdSpirvSpecialization $changed.bytes @{}
Check-Analysis ($roundtrip.effective['13'].value -eq $true -and $roundtrip.effective['16'].value -eq -17) 'patched binary roundtrip'
Check-Analysis ($changed.bytes.Length -eq $bytes.Length) 'instruction widths preserved'
Check-Analysis ($bytes[24] -eq $changed.bytes[24]) 'unknown float8 capability preserved'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization ([byte[]]::new(4)) @{} } 'short header'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization ($bytes[0..($bytes.Length-2)]) @{} } 'unaligned bytes'
$bad = $fixture.Clone(); $bad[0] = 0
Expect-AnalysisReject { Convert-AmdSpirvSpecialization (Words-ToBytes $bad) @{} } 'bad magic'
$bad = $fixture.Clone(); $bad[5] = 0
Expect-AnalysisReject { Convert-AmdSpirvSpecialization (Words-ToBytes $bad) @{} } 'zero word count'
$bad = $fixture.Clone(); $bad[5] = 4294901777
Expect-AnalysisReject { Convert-AmdSpirvSpecialization (Words-ToBytes $bad) @{} } 'instruction past EOF'
$bad = $fixture.Clone(); $bad[4] = 1
Expect-AnalysisReject { Convert-AmdSpirvSpecialization (Words-ToBytes $bad) @{} } 'reserved schema'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{99=16} } 'unknown specialization'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{x=16} } 'invalid specialization ID'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{10=-1} } 'negative uint'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{10=4294967296} } 'uint overflow'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{10=1.5} } 'fractional uint'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{13='false'} } 'ambiguous boolean string'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{15=[double]::NaN} } 'nonfinite float'
Expect-AnalysisReject { Convert-AmdSpirvSpecialization $bytes @{16=2147483648} } 'signed overflow'
Expect-AnalysisReject { Get-AmdMetadataNumber '.wavefront_size: 64' 'lds_size' } 'missing ELF field'
Expect-AnalysisReject { Get-AmdMetadataNumber ".lds_size: 12`n.lds_size: 24" 'lds_size' } 'ambiguous ELF field'
Check-Analysis ((Get-AmdMetadataNumber '.wavefront_size: 32' 'wavefront_size') -eq 32) 'ELF numeric extraction'
Write-Host "AMD offline analysis parser: $checks CPU checks PASS (no compiler/GPU)."
