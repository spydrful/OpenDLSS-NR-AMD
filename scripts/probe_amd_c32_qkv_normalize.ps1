# Standalone C32 QKV -> normalization diagnostics. MIT; default is CPU-only.
[CmdletBinding()]
param(
  [ValidateSet('Build','Bounded','Capture','Check','Timing')][string]$Mode = 'Build',
  [switch]$Run,
  [string]$OutputDirectory = ('build/c32-qkv-diagnostic-' + [Guid]::NewGuid().ToString('N')),
  [string]$BuildDirectory,
  [string]$Model,
  [string]$ActualInputs,
  [string]$StrictReport
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
function Resolve-TaskPath([string]$Path) {
  if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
  return [IO.Path]::GetFullPath((Join-Path $taskRoot $Path))
}
function Assert-PrivateBuildPath([string]$Path) {
  $taskPrefix = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
  if (-not $Path.StartsWith($taskPrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Diagnostic output/build paths must stay under the repository build directory' }
}
function Get-TaskHash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Write-TaskJson([string]$Path,$Value) { [IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 30)+"`n",[Text.UTF8Encoding]::new($false)) }
function Invoke-TaskProgram([string]$Executable,[string[]]$Arguments,[string]$Log) {
  & $Executable @Arguments *> $Log
  if ($LASTEXITCODE -ne 0) { throw "Diagnostic program failed (exit $LASTEXITCODE); preserved log: $Log" }
}
if (($Mode -eq 'Build' -and $Run) -or ($Mode -ne 'Build' -and -not $Run)) { throw 'Build is CPU-only; every GPU mode requires an explicit -Run' }
if ($Mode -eq 'Build' -and ($BuildDirectory -or $Model -or $ActualInputs -or $StrictReport)) { throw 'CPU-only Build does not accept GPU assets or a previous build directory' }
if ($Mode -ne 'Build' -and -not $BuildDirectory) { throw 'GPU diagnostics require -BuildDirectory from a completed CPU-only Build' }
if ($Mode -in @('Capture','Check','Timing') -and -not $Model) { throw "$Mode requires a locally imported -Model" }
if ($Mode -in @('Check','Timing') -and -not $ActualInputs) { throw "$Mode requires -ActualInputs from local Capture" }
if ($Mode -eq 'Timing' -and -not $StrictReport) { throw 'Timing requires the matching passed -StrictReport from Check' }
if ($Mode -notin @('Check','Timing') -and $ActualInputs) { throw 'ActualInputs is valid only for Check or Timing' }
if ($Mode -ne 'Timing' -and $StrictReport) { throw 'StrictReport is valid only for Timing' }
$taskOut = Resolve-TaskPath $OutputDirectory
Assert-PrivateBuildPath $taskOut
if (Test-Path -LiteralPath $taskOut) { throw 'Choose a fresh output directory; existing diagnostic evidence is never overwritten' }

if ($Mode -eq 'Build') {
  New-Item -ItemType Directory -Path $taskOut,(Join-Path $taskOut 'obj'),(Join-Path $taskOut 'shaders'),(Join-Path $taskOut 'baseline-shaders') | Out-Null
  $taskOwned = @('tests/amd_c32_qkv_normalize_probe.cpp','tests/amd_c32_qkv_normalize_timing.cpp','tests/amd_c32_qkv_normalize_capture.cpp','shaders/diagnostic/amd_c32_qkv_normalize_capture.comp','scripts/probe_amd_c32_qkv_normalize.ps1')
  $taskSourcePaths = @($taskOwned)
  $taskSourcePaths += @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'src') -File | ForEach-Object { 'src/'+$_.Name })
  $taskSourcePaths += @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'shaders') -File | Where-Object { $_.Extension -in @('.comp','.glsl') } | ForEach-Object { 'shaders/'+$_.Name })
  $taskSourcePaths += @('tests/native_selftest.cpp','tests/model_validation.cpp','tests/composite_validation.cpp','tests/amd_kernel_preservation.cpp','tools/volk/volk.c','tools/volk/volk.h','scripts/build.ps1','scripts/build_shaders.ps1','scripts/find_vcvars.ps1','scripts/complete_amd_silu_float_controls.ps1')
  $taskSourcePaths = @($taskSourcePaths | Select-Object -Unique)
  $taskSources = [ordered]@{}
  foreach ($taskSource in $taskSourcePaths) {
    $taskFile = Join-Path $taskRoot $taskSource
    $taskSources[$taskSource] = Get-TaskHash $taskFile
    $taskDestination = Join-Path $taskOut ('source-closure/'+$taskSource)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $taskDestination) | Out-Null
    Copy-Item -LiteralPath $taskFile -Destination $taskDestination
  }
  # Compile the normal application into a new directory for generated inputs.
  # This does not touch any existing release/build directory or run Vulkan.
  & (Join-Path $PSScriptRoot 'build.ps1') -Backend amd -OutputDirectory (Join-Path $taskOut 'application') *> (Join-Path $taskOut 'application-build.log')
  if ($LASTEXITCODE -ne 0) { throw 'Source-built diagnostic application failed; inspect application-build.log' }
  foreach ($taskName in @('amd_gemm_direct_rte_pair','amd_window_normalize')) {
    Copy-Item -LiteralPath (Join-Path $taskOut ('application/shaders/'+$taskName+'.spv')) -Destination (Join-Path $taskOut ('baseline-shaders/'+$taskName+'.spv'))
  }
  Copy-Item -LiteralPath (Join-Path $taskOut 'application/shaders/amd_qkv32_normalize_wave6.spv') -Destination (Join-Path $taskOut 'shaders/amd_qkv32_normalize_wave6.spv')
  $taskGlslang = Join-Path $taskRoot 'tools/glslang/bin/glslang.exe'
  $taskCaptureSpv = Join-Path $taskOut 'shaders/amd_qkv32_normalize_wave6_capture.spv'
  & $taskGlslang -V --target-env vulkan1.3 ('-I'+(Join-Path $taskOut 'source-closure/shaders')) (Join-Path $taskOut 'source-closure/shaders/diagnostic/amd_c32_qkv_normalize_capture.comp') -o $taskCaptureSpv *> (Join-Path $taskOut 'capture-shader-build.log')
  if ($LASTEXITCODE -ne 0) { throw 'Nonshipping capture shader compilation failed' }
  $taskClosure = & (Join-Path $PSScriptRoot 'complete_amd_silu_float_controls.ps1') -InputPath $taskCaptureSpv -OutputPath $taskCaptureSpv
  $taskModules = [ordered]@{}
  foreach ($taskModule in @('baseline-shaders/amd_gemm_direct_rte_pair.spv','baseline-shaders/amd_window_normalize.spv','shaders/amd_qkv32_normalize_wave6.spv','shaders/amd_qkv32_normalize_wave6_capture.spv')) { $taskModules[$taskModule] = Get-TaskHash (Join-Path $taskOut $taskModule) }
  # This filtering is solely in the diagnostic copy. The exact function anchor,
  # source delta and generated source identity are retained in build metadata.
  $taskGraphPath = Join-Path $taskOut 'source-closure/src/nr_graph.cpp'
  $taskGraph = [IO.File]::ReadAllText($taskGraphPath)
  $taskAnchor = "void Graph::capture(VkCommandBuffer commands, const std::string& name, const Activation& source) {`n  if (!options_.captureBoundaries) return;"
  $taskNormalized = $taskGraph.Replace("`r`n","`n")
  if (($taskNormalized.Split(@($taskAnchor),[StringSplitOptions]::None)).Count -ne 2) { throw 'Selective graph filter requires exactly one unchanged capture-function anchor' }
  $taskNames = @()
  foreach ($taskBlock in @(0,1,2,3,4,66,67,68,69,70)) { foreach ($taskBoundary in @('ffnQuantized','qkv','normalized')) { $taskNames += ('block-'+$taskBlock+'/'+$taskBoundary) } }
  $taskFilter = "`n  // Standalone C32 diagnostic filter; inference statements are unchanged.`n  const char* diagnosticNames[] = {" + (($taskNames|ForEach-Object {'"'+$_+'"'}) -join ',') + "};`n  bool diagnosticSelected = false;`n  for (const char* selected : diagnosticNames) diagnosticSelected |= name == selected;`n  if (!diagnosticSelected) return;"
  $taskGeneratedGraph = $taskNormalized.Replace($taskAnchor,$taskAnchor+$taskFilter)
  $taskGenerated = Join-Path $taskOut 'nr_graph_c32_capture.cpp'
  [IO.File]::WriteAllText($taskGenerated,$taskGeneratedGraph,[Text.UTF8Encoding]::new($false))
  if ($taskGeneratedGraph.Replace($taskAnchor+$taskFilter,$taskAnchor) -cne $taskNormalized) { throw 'Diagnostic graph patch changes more than the selective filter' }
  $taskVcvars = (& (Join-Path $PSScriptRoot 'find_vcvars.ps1') | Select-Object -First 1).Trim()
  $taskCompilerPath = ((cmd /c ('"'+$taskVcvars+'" >nul && where cl')) | Select-Object -First 1).Trim()
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $taskCompilerPath -PathType Leaf)) { throw 'Could not identify the actual MSVC compiler' }
  $taskCompilerVersion = (Get-Item -LiteralPath $taskCompilerPath).VersionInfo
  $taskIncludes = '/I"'+(Join-Path $taskOut 'source-closure/src')+'" /I"'+(Join-Path $taskOut 'source-closure/tools/volk')+'" /I"'+(Join-Path $taskRoot 'tools/Vulkan-Headers/include')+'"'
  $taskExecutables = [ordered]@{}
  $taskCompileCommands = [ordered]@{}
  foreach ($taskName in @('probe','timing','capture')) {
    $taskExeName = 'amd_c32_qkv_normalize_'+$taskName+'.exe'
    $taskCompileSources = @((Join-Path $taskOut ('source-closure/tests/amd_c32_qkv_normalize_'+$taskName+'.cpp')),(Join-Path $taskOut 'source-closure/src/vk_context.cpp'),(Join-Path $taskOut 'source-closure/src/nr_model.cpp'),(Join-Path $taskOut 'source-closure/tools/volk/volk.c'))
    if ($taskName -eq 'capture') { $taskCompileSources += @($taskGenerated,(Join-Path $taskOut 'source-closure/src/kernels.cpp'),(Join-Path $taskOut 'source-closure/src/reference.cpp')) }
    $taskObjectDirectory = Join-Path $taskOut ('obj/'+$taskName)
    New-Item -ItemType Directory -Path $taskObjectDirectory | Out-Null
    $taskSourcesText = ($taskCompileSources|ForEach-Object {'"'+$_+'"'}) -join ' '
    $taskCommand = '"'+$taskVcvars+'" >nul && cl /nologo /std:c++20 /EHsc /W3 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DVK_ENABLE_BETA_EXTENSIONS '+$taskIncludes+' /Fo"'+$taskObjectDirectory+'/" '+$taskSourcesText+' /Fe:"'+(Join-Path $taskOut $taskExeName)+'" /link /SUBSYSTEM:CONSOLE'
    $taskCompileCommands[$taskExeName] = $taskCommand
    cmd /c $taskCommand *> (Join-Path $taskOut ($taskName+'-build.log'))
    if ($LASTEXITCODE -ne 0) { throw "Standalone $taskName compilation failed; inspect $taskName-build.log" }
    $taskExecutables[$taskExeName] = Get-TaskHash (Join-Path $taskOut $taskExeName)
  }
  foreach ($taskSource in $taskSourcePaths) { if ((Get-TaskHash (Join-Path $taskRoot $taskSource)) -ne $taskSources[$taskSource]) { throw "Source changed during diagnostic build: $taskSource" } }
  $taskBuild = [ordered]@{
    format='OpenNR-c32-qkv-diagnostic-build-v1'; createdUtc=[DateTime]::UtcNow.ToString('o'); gpuExecuted=$false
    sourceFiles=$taskSources; modules=$taskModules; executables=$taskExecutables; compileCommands=$taskCompileCommands
    applicationExecutableSha256=(Get-TaskHash (Join-Path $taskOut 'application/dlss5vk.exe'))
    glslangSha256=(Get-TaskHash $taskGlslang); vcvarsPath=$taskVcvars; floatControlClosure=$taskClosure
    compiler=[ordered]@{path=$taskCompilerPath; sha256=(Get-TaskHash $taskCompilerPath); fileVersion=$taskCompilerVersion.FileVersion; productVersion=$taskCompilerVersion.ProductVersion}; sdkIndependentlyFrozen=$false
    diagnosticGraph=[ordered]@{originalSourceSha256=$taskSources['src/nr_graph.cpp']; generatedSourceSha256=(Get-TaskHash $taskGenerated); originalNewlinesNormalizedToLf=$true; onlyInsertion=$taskFilter; names=$taskNames; shippingSourceChanged=$false}
    supportedModelManifestSha256='163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e'; timingDevice='1002:7550'; timingDriver='AMD proprietary driver|26.9.1 (LLPC)|8389003'
    tunerProofSchemaSupported=$false; shippingCaptureShader=$false; tensorsIncluded=$false
  }
  Write-TaskJson (Join-Path $taskOut 'diagnostic-build.json') $taskBuild
  foreach ($taskName in @('probe','timing','capture')) { Invoke-TaskProgram (Join-Path $taskOut ('amd_c32_qkv_normalize_'+$taskName+'.exe')) @('--cpu-check') (Join-Path $taskOut ($taskName+'-cpu.log')) }
  Write-Output "CPU build/checks passed; no Vulkan execution. BuildDirectory: $taskOut"
  return
}

$taskBuildDirectory = Resolve-TaskPath $BuildDirectory
Assert-PrivateBuildPath $taskBuildDirectory
$taskBuildPath = Join-Path $taskBuildDirectory 'diagnostic-build.json'
$taskBuildRecord = Get-Content -LiteralPath $taskBuildPath -Raw | ConvertFrom-Json
if ($taskBuildRecord.format -ne 'OpenNR-c32-qkv-diagnostic-build-v1') { throw 'Unsupported diagnostic build manifest' }
foreach ($taskExe in $taskBuildRecord.executables.psobject.Properties) { if ((Get-TaskHash (Join-Path $taskBuildDirectory $taskExe.Name)) -ne $taskExe.Value) { throw 'Diagnostic executable changed after CPU build' } }
foreach ($taskModule in $taskBuildRecord.modules.psobject.Properties) { if ((Get-TaskHash (Join-Path $taskBuildDirectory $taskModule.Name)) -ne $taskModule.Value) { throw 'Diagnostic module changed after CPU build' } }
if ((Get-TaskHash (Join-Path $taskBuildDirectory 'application/dlss5vk.exe')) -ne $taskBuildRecord.applicationExecutableSha256) { throw 'Generated-input application changed after CPU build' }
foreach ($taskName in @('probe','timing','capture')) { if ((Get-Content -LiteralPath (Join-Path $taskBuildDirectory ($taskName+'-cpu.log')) -Raw) -notmatch 'PASS:\d+; no Vulkan context') { throw 'Diagnostic build CPU checks missing' } }
$taskModelPath = if ($Model) { Resolve-TaskPath $Model } else { $null }
if ($taskModelPath -and (Get-TaskHash (Join-Path $taskModelPath 'manifest.json')) -ne $taskBuildRecord.supportedModelManifestSha256) { throw 'Supported local model manifest mismatch' }
$taskInputsPath = if ($ActualInputs) { Resolve-TaskPath $ActualInputs } else { $null }
$taskStrictPath = if ($StrictReport) { Resolve-TaskPath $StrictReport } else { $null }
New-Item -ItemType Directory -Path $taskOut | Out-Null
$taskSavedEnvironment = @{}
foreach ($taskItem in Get-ChildItem Env: | Where-Object { $_.Name.StartsWith('DLSS5VK_AMD_') -or $_.Name -eq 'DLSS5VK_PIPELINE_CACHE' -or $_.Name -eq 'DLSS5VK_BACKEND' }) { $taskSavedEnvironment[$taskItem.Name]=$taskItem.Value; [Environment]::SetEnvironmentVariable($taskItem.Name,$null,'Process') }
$taskLaunches = @()
try {
  $taskResult = Join-Path $taskOut 'result'
  if ($Mode -eq 'Capture') {
    Write-Host 'Explicit GPU capture: thirty locally generated network tensors require about 2.12 GB disk plus features/head/logs and graph GPU allocations. These outputs remain under ignored build/.'
    $taskAnchorPath = Join-Path $taskOut 'anchor'
    $taskAppArguments = @('modelcheck','--backend','amd','--model',$taskModelPath,'--shaders',(Join-Path $taskBuildDirectory 'application/shaders'),'--amd-kernels','optimized','--amd-arithmetic','k16','--amd-gemm','direct-rte-pair','--amd-tile-n','16','--amd-stage-k','16','--amd-window-layout','arena-rte','--amd-window-queries','32','--amd-qkv-normalize','off','--amd-fusion','0','--amd-ffn32-fusion','0','--amd-qkv32-fusion','0','--amd-expert-fusion','0','--amd-block-fusion','0','--amd-hardware-publication','0','--width','1707','--height','960','--frames','1','--head-only','--fixture',$taskAnchorPath)
    $taskApp = Join-Path $taskBuildDirectory 'application/dlss5vk.exe'
    $taskLaunches += [ordered]@{executable=$taskApp; arguments=$taskAppArguments; executableSha256=(Get-TaskHash $taskApp)}
    Invoke-TaskProgram $taskApp $taskAppArguments (Join-Path $taskOut 'anchor.log')
    $taskExe = Join-Path $taskBuildDirectory 'amd_c32_qkv_normalize_capture.exe'
    $taskArguments = @('--run','--model',$taskModelPath,'--features',(Join-Path $taskAnchorPath 'features.f32'),'--prior-head',(Join-Path $taskAnchorPath 'head.f32'),'--prior-report',(Join-Path $taskAnchorPath 'modelcheck-report.json'),'--shaders',(Join-Path $taskBuildDirectory 'application/shaders'),'--output',$taskResult)
  } else {
    $taskExe = Join-Path $taskBuildDirectory ('amd_c32_qkv_normalize_'+$(if($Mode -eq 'Timing'){'timing'}else{'probe'})+'.exe')
    $taskArguments = @('--run','--output',$taskResult)
    if ($taskModelPath) { $taskArguments += @('--model',$taskModelPath) }
    if ($taskInputsPath) { $taskArguments += @('--actual-inputs',$taskInputsPath) }
    if ($taskStrictPath) { $taskArguments += @('--strict-report',$taskStrictPath) }
  }
  $taskLaunches += [ordered]@{executable=$taskExe; arguments=$taskArguments; executableSha256=(Get-TaskHash $taskExe)}
  Invoke-TaskProgram $taskExe $taskArguments (Join-Path $taskOut 'run.log')
  Write-Output "Diagnostic $Mode completed: $taskResult"
} finally {
  foreach ($taskItem in Get-ChildItem Env: | Where-Object { $_.Name.StartsWith('DLSS5VK_AMD_') -or $_.Name -eq 'DLSS5VK_PIPELINE_CACHE' -or $_.Name -eq 'DLSS5VK_BACKEND' }) { [Environment]::SetEnvironmentVariable($taskItem.Name,$null,'Process') }
  foreach ($taskKey in $taskSavedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($taskKey,$taskSavedEnvironment[$taskKey],'Process') }
  Write-TaskJson (Join-Path $taskOut 'launch.json') ([ordered]@{format='OpenNR-c32-qkv-diagnostic-launch-v1'; mode=$Mode; gpuExplicitlyRequested=[bool]$Run; buildManifestSha256=(Get-TaskHash $taskBuildPath); launches=$taskLaunches; localModelManifestSha256=$(if($taskModelPath){Get-TaskHash (Join-Path $taskModelPath 'manifest.json')}else{$null}); inputCatalogSha256=$(if($taskInputsPath){Get-TaskHash $taskInputsPath}else{$null}); strictReportSha256=$(if($taskStrictPath){Get-TaskHash $taskStrictPath}else{$null}); defaultOrShippingSelectionChanged=$false})
}
