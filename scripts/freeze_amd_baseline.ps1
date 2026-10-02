[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [Parameter(Mandatory=$true)][string]$ModelDirectory,
  [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dlss5vk.exe'),
  [string]$RuntimeDll=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\game\OpenNrRuntime.dll'),
  [string]$ShaderDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\shaders'),
  [string]$CxxCompiler
)
$ErrorActionPreference='Stop'
function Get-NrFrozenSourceIdentity([string]$SourceRoot) {
  $taskSourceRoot=[IO.Path]::GetFullPath($SourceRoot).TrimEnd('\','/')
  $taskSourceScopes=@('src','shaders','game','tests','scripts')
  $taskSourceExtensions=@('.cpp','.cc','.c','.h','.hpp','.inl','.inc','.comp','.glsl','.hlsl','.vert','.frag','.wgsl','.ps1','.py','.json','.cmake')
  $taskIgnoredDirectories=@('.git','build','dist','obj','__pycache__','node_modules','models','model','fixtures','captures','.cache')
  $taskSourceFiles=@()
  $taskPending=[Collections.Generic.Stack[string]]::new()
  foreach($taskScope in $taskSourceScopes){
    $taskScopePath=Join-Path $taskSourceRoot $taskScope
    if(Test-Path -LiteralPath $taskScopePath -PathType Container){$taskPending.Push($taskScopePath)}
  }
  while($taskPending.Count){
    foreach($taskItem in Get-ChildItem -LiteralPath $taskPending.Pop() -Force){
      if($taskItem.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Source identity contains a reparse point'}
      if($taskItem.PSIsContainer){if($taskItem.Name -notin $taskIgnoredDirectories){$taskPending.Push($taskItem.FullName)};continue}
      if($taskItem.Extension -notin $taskSourceExtensions -and $taskItem.Name -ne 'CMakeLists.txt'){continue}
      $taskRelative=$taskItem.FullName.Substring($taskSourceRoot.Length).TrimStart('\','/').Replace('\','/')
      $taskSourceFiles+=[ordered]@{path=$taskRelative;sha256=(Get-FileHash -LiteralPath $taskItem.FullName -Algorithm SHA256).Hash.ToLowerInvariant();length=$taskItem.Length}
    }
  }
  $taskSourceFiles=@($taskSourceFiles | Sort-Object -Property path -CaseSensitive)
  $taskSourceLines=@($taskSourceFiles | ForEach-Object {'{0}:{1}' -f $_.path,$_.sha256})
  $taskIdentityBytes=[Text.Encoding]::UTF8.GetBytes(($taskSourceLines -join "`n")+"`n")
  $taskHasher=[Security.Cryptography.SHA256]::Create()
  try{$taskSourceHash=[BitConverter]::ToString($taskHasher.ComputeHash($taskIdentityBytes)).Replace('-','').ToLowerInvariant()}finally{$taskHasher.Dispose()}
  [ordered]@{sha256=$taskSourceHash;scopes=$taskSourceScopes;extensions=$taskSourceExtensions;files=$taskSourceFiles;sourceBytesCopied=$false;binaryCorrespondenceEstablished=$false}
}
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskOutput=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $taskOutput){throw 'Baseline destination exists; snapshots are never overwritten'}
$taskInputs=@([IO.Path]::GetFullPath($Executable),[IO.Path]::GetFullPath($RuntimeDll))
$taskShaders=@(Get-ChildItem -LiteralPath ([IO.Path]::GetFullPath($ShaderDirectory)) -Filter '*.spv' -File)
if(-not $taskShaders.Count){throw 'No baseline shaders'}
foreach($taskInput in $taskInputs){if(-not(Test-Path -LiteralPath $taskInput -PathType Leaf)){throw "Missing baseline binary: $taskInput"}}
$taskManifest=Join-Path ([IO.Path]::GetFullPath($ModelDirectory)) 'manifest.json'
$taskModelHash=(Get-FileHash -LiteralPath $taskManifest -Algorithm SHA256).Hash.ToLowerInvariant()
$taskCommit=(& git -c "safe.directory=$($taskRoot.Replace('\','/'))" -C $taskRoot rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0){throw 'Cannot identify source commit'}
$taskDirty=@(& git -c "safe.directory=$($taskRoot.Replace('\','/'))" -C $taskRoot status --porcelain)
if($LASTEXITCODE -ne 0){throw 'Cannot identify source state'}
$taskSourceIdentity=Get-NrFrozenSourceIdentity $taskRoot
$taskCxxCompilerPath=$null
if($CxxCompiler){
  $taskCxxCompilerPath=[IO.Path]::GetFullPath($CxxCompiler)
  if(-not(Test-Path -LiteralPath $taskCxxCompilerPath -PathType Leaf)){throw 'Explicit C++ compiler path does not exist'}
}else{
  $taskClCommand=Get-Command cl -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if($taskClCommand){$taskCxxCompilerPath=$taskClCommand.Source}
}
$taskCxxCompilerIdentity=if($taskCxxCompilerPath){
  $taskCompilerVersion=(Get-Item -LiteralPath $taskCxxCompilerPath).VersionInfo
  [ordered]@{path=$taskCxxCompilerPath;sha256=(Get-FileHash -LiteralPath $taskCxxCompilerPath -Algorithm SHA256).Hash.ToLowerInvariant();version=$taskCompilerVersion.FileVersion;productVersion=$taskCompilerVersion.ProductVersion;versionSource='file version resource'}
}else{$null}
New-Item -ItemType Directory -Path (Join-Path $taskOutput 'shaders') -Force | Out-Null
$taskFiles=@()
foreach($taskInput in $taskInputs){
  $taskTarget=Join-Path $taskOutput ([IO.Path]::GetFileName($taskInput));Copy-Item -LiteralPath $taskInput -Destination $taskTarget
  $taskFiles+=@{path=[IO.Path]::GetFileName($taskInput);sha256=(Get-FileHash -LiteralPath $taskTarget -Algorithm SHA256).Hash.ToLowerInvariant();length=(Get-Item -LiteralPath $taskTarget).Length}
}
foreach($taskShader in $taskShaders){
  $taskTarget=Join-Path $taskOutput ('shaders\'+$taskShader.Name);Copy-Item -LiteralPath $taskShader.FullName -Destination $taskTarget
  $taskFiles+=@{path='shaders/'+$taskShader.Name;sha256=(Get-FileHash -LiteralPath $taskTarget -Algorithm SHA256).Hash.ToLowerInvariant();length=$taskShader.Length}
}
$taskTuningInput=Join-Path ([IO.Path]::GetFullPath($ShaderDirectory)) 'amd-tuning.json'
if(Test-Path -LiteralPath $taskTuningInput -PathType Leaf){
  $taskTuningTarget=Join-Path $taskOutput 'shaders\amd-tuning.json'
  Copy-Item -LiteralPath $taskTuningInput -Destination $taskTuningTarget
  $taskFiles+=@{path='shaders/amd-tuning.json';sha256=(Get-FileHash -LiteralPath $taskTuningTarget -Algorithm SHA256).Hash.ToLowerInvariant();length=(Get-Item -LiteralPath $taskTuningTarget).Length}
}
$taskDevice=(& $Executable info --backend amd 2>&1 | Out-String)
if($LASTEXITCODE -ne 0){throw 'Baseline device query failed'}
$taskCompiler=Join-Path $taskRoot 'tools\glslang\bin\glslang.exe'
$taskCompilerIdentity=if(Test-Path -LiteralPath $taskCompiler){@{sha256=(Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash.ToLowerInvariant();version=(& $taskCompiler --version | Out-String)}}else{$null}
@{format='OpenNR-frozen-baseline-v1';createdUtc=[DateTime]::UtcNow.ToString('o');sourceCommit=$taskCommit;sourceDirty=($taskDirty.Count -gt 0);sourceInputs=$taskSourceIdentity;modelManifestSha256=$taskModelHash;deviceReport=$taskDevice;compiler=$taskCompilerIdentity;cxxCompiler=$taskCxxCompilerIdentity;files=$taskFiles;weightsIncluded=$false;performanceCapturedSeparately=$true} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskOutput 'identity.json') -Encoding utf8
Write-Host "Frozen baseline: $taskOutput"
