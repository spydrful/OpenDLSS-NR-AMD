param([switch]$Test, [switch]$Debug)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = Join-Path $taskRoot 'build\importer'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskVcvars = & (Join-Path $PSScriptRoot 'find_vcvars.ps1')
$taskImporter = Join-Path $taskRoot 'tools\model_importer.cpp'
$taskOptions = if ($Debug) { '/Od /Zi' } else { '/O2' }
$taskCommand = '"' + $taskVcvars + '" >nul && cl /nologo /std:c++20 /EHsc /W4 ' + $taskOptions + ' /D_CRT_SECURE_NO_WARNINGS /Fo"' + $taskOutput + '\\" "' + $taskImporter + '" /Fe:"' + $taskOutput + '\model_importer.exe" /link /SUBSYSTEM:CONSOLE'
cmd /c $taskCommand
if ($LASTEXITCODE -ne 0) { throw 'model importer compilation failed' }
if ($Test) {
  $taskTests = Join-Path $taskRoot 'tests\model_importer_tests.cpp'
  $taskCommand = '"' + $taskVcvars + '" >nul && cl /nologo /std:c++20 /EHsc /W4 ' + $taskOptions + ' /D_CRT_SECURE_NO_WARNINGS /DNR_IMPORTER_NO_MAIN /Fo"' + $taskOutput + '\\" "' + $taskImporter + '" "' + $taskTests + '" /Fe:"' + $taskOutput + '\model_importer_tests.exe" /link /SUBSYSTEM:CONSOLE'
  cmd /c $taskCommand
  if ($LASTEXITCODE -ne 0) { throw 'model importer test compilation failed' }
  & (Join-Path $taskOutput 'model_importer_tests.exe')
  if ($LASTEXITCODE -ne 0) { throw 'model importer tests failed' }
}
Write-Host "Built $(Join-Path $taskOutput 'model_importer.exe')"
