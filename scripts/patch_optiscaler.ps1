$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$hostRoot = Join-Path $root 'third_party\optiscaler-host'
function Replace-Once([string]$file, [string]$old, [string]$new) {
  $path = Join-Path $hostRoot $file
  $text = [IO.File]::ReadAllText($path).Replace("`r`n","`n")
  $old = $old.Replace("`r`n","`n")
  $new = $new.Replace("`r`n","`n")
  if ($text.Contains($new)) { return }
  if (-not $text.Contains($old)) { throw "Pinned source mismatch in $file" }
  [IO.File]::WriteAllText($path, $text.Replace($old,$new), [Text.UTF8Encoding]::new($false))
}
function Replace-Section([string]$file, [string]$startToken, [string]$endToken, [string]$snippet) {
  $path = Join-Path $hostRoot $file
  $text = [IO.File]::ReadAllText($path).Replace("`r`n","`n")
  $start = $text.IndexOf($startToken)
  $end = if ($start -ge 0) { $text.IndexOf($endToken,$start + $startToken.Length) } else { -1 }
  if ($start -lt 0 -or $end -lt 0) { throw "Pinned function boundary mismatch in $file ($startToken)" }
  $body = [IO.File]::ReadAllText((Join-Path $root "integrations\optiscaler\$snippet")).Replace("`r`n","`n")
  $body = $body.Substring($body.IndexOf($startToken))
  $updated = $text.Substring(0,$start) + $body + $text.Substring($end)
  if ($updated -ne $text) { [IO.File]::WriteAllText($path, $updated, [Text.UTF8Encoding]::new($false)) }
}
$header = [IO.File]::ReadAllText((Join-Path $root 'game\open_nr_api.h')).Replace('"compat/LmxxfNrApi.h"','"lmxxf_runtime/LmxxfNrApi.h"')
[IO.File]::WriteAllText((Join-Path $hostRoot 'OptiScaler\dlssnr\backend\OpenNrApi.h'),$header,[Text.UTF8Encoding]::new($false))
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '#include "lmxxf_runtime/LmxxfNrApi.h"' "#include `"lmxxf_runtime/LmxxfNrApi.h`"`r`n#include `"OpenNrApi.h`""
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '    LmxxfNrApi table {};' "    LmxxfNrApi table {};`r`n    int32_t (*setFrameMetadata)(void*, const OpenNrFrameMetadata*) = nullptr;"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '    int32_t (*setFrameMetadata)(void*, const OpenNrFrameMetadata*) = nullptr;' "    int32_t (*setFrameMetadata)(void*, const OpenNrFrameMetadata*) = nullptr;`n    LmxxfCut::RecoverSubmissionFn recoverSubmission = nullptr;"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '        api->setExposure = reinterpret_cast<PFN_MochizukiNrSetExposure>(GetProcAddress(dll, "MochizukiNrSetExposure"));' "        api->setExposure = reinterpret_cast<PFN_MochizukiNrSetExposure>(GetProcAddress(dll, `"MochizukiNrSetExposure`"));`r`n        api->setFrameMetadata = reinterpret_cast<int32_t (*)(void*, const OpenNrFrameMetadata*)>(GetProcAddress(dll, `"OpenNrSetFrameMetadata`"));"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '        api->setFrameMetadata = reinterpret_cast<int32_t (*)(void*, const OpenNrFrameMetadata*)>(GetProcAddress(dll, "OpenNrSetFrameMetadata"));' "        api->setFrameMetadata = reinterpret_cast<int32_t (*)(void*, const OpenNrFrameMetadata*)>(GetProcAddress(dll, `"OpenNrSetFrameMetadata`"));`n        api->recoverSubmission = reinterpret_cast<LmxxfCut::RecoverSubmissionFn>(GetProcAddress(dll, `"OpenNrRecoverSubmission`"));"
$metadata = @'
        if (api->setFrameMetadata)
        {
            OpenNrFrameMetadata metadata {};
            metadata.struct_size = sizeof(metadata);
            metadata.abi_version = OPEN_NR_ABI_VERSION;
            metadata.exposure = frame.exposure;
            metadata.exposure_state = static_cast<uint32_t>(frame.exposureState);
            metadata.pre_exposure = frame.preExposure;
            metadata.exposure_scale = frame.exposureScale;
            metadata.color_x = frame.colourX;
            metadata.color_y = frame.colourY;
            metadata.motion_x = frame.motionX;
            metadata.motion_y = frame.motionY;
            metadata.jitter_x = frame.jitterX;
            metadata.jitter_y = frame.jitterY;
            if (api->setFrameMetadata(session, &metadata) != LMXXF_NR_OK)
                return declineFrame("OpenDLSS AMD: invalid frame metadata");
        }
        else if (api->setExposure)
            api->setExposure(session, GameExposure(cmd, frame));
'@
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' "        if (api->setExposure)`r`n            api->setExposure(session, GameExposure(cmd, frame));" ($metadata.Replace("`n","`r`n"))
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '    fi.color = frame.colour;' "    fi.color = frame.colour;`r`n    fi.reset = frame.reset ? 1u : 0u;"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '    FillMochizukiControls(*Config::Instance(), c);' "    FillMochizukiControls(*Config::Instance(), c);`r`n    if (api->setFrameMetadata) { c.drs_mode = 0; c.linear_input = 1; }"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' '    LmxxfNrJob job {};' "    if (api->setFrameMetadata) fi.smooth_strength = 0;`r`n    LmxxfNrJob job {};"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' 'const int32_t frameRc = api->table.PrepareFrame(session, &fi, &job);' "fi.flags = HostFrameFlags(fi.flags, api->setFrameMetadata != nullptr);`n                const int32_t frameRc = api->table.PrepareFrame(session, &fi, &job);"
Replace-Once 'OptiScaler\dlssnr\amd\AmdPreSr.h' '    UINT width = 0, height = 0;' "    UINT width = 0, height = 0;`r`n    UINT colourX = 0, colourY = 0, motionX = 0, motionY = 0;"
foreach ($f in 'OptiScaler\dlssnr\amd\AmdBridge.cpp','OptiScaler\dlssnr\DlssNr_Menu.cpp') { Replace-Once $f 'MochizukiNrRuntime.dll' 'OpenNrRuntime.dll' }
$originOld = @'
    if (x || y)
    {
        Message("AMD pre-SR: nonzero colour subrect origin unsupported");
        return true;
    }
'@
$originNew = @'
    f.colourX = x;
    f.colourY = y;
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &f.motionX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &f.motionY);
    if ((x || y || f.motionX || f.motionY) && active != DlssNr::Backend::Kind::Mochizuki)
    {
        Message("AMD pre-SR: this backend cannot read a nonzero subrect origin");
        return true;
    }
'@
Replace-Once 'OptiScaler\dlssnr\amd\AmdBridge.cpp' ($originOld.Replace("`n","`r`n")) ($originNew.Replace("`n","`r`n"))
# NR changes private compute bindings. Record against the real segment and
# restore the captured game bindings so the logical continuation remains valid.
Replace-Once 'OptiScaler\dlssnr\submission\CommandListProxy.h' '    virtual HRESULT STDMETHODCALLTYPE SplitSegments(void) = 0;' "    virtual HRESULT STDMETHODCALLTYPE SplitSegments(void) = 0;`r`n    virtual ID3D12GraphicsCommandList* STDMETHODCALLTYPE RuntimeSegment(void) = 0;`r`n    virtual void STDMETHODCALLTYPE RestoreRuntimeBindings(void) = 0;"
Replace-Once 'OptiScaler\dlssnr\submission\CommandListProxy.h' '    HRESULT STDMETHODCALLTYPE SplitSegments() override' "    ID3D12GraphicsCommandList* STDMETHODCALLTYPE RuntimeSegment() override { return logical.Current(); }`r`n    void STDMETHODCALLTYPE RestoreRuntimeBindings() override { if (logical.Current()) contState.ApplyTo(logical.Current()); }`r`n    HRESULT STDMETHODCALLTYPE SplitSegments() override"
[IO.File]::Copy((Join-Path $root 'integrations\optiscaler\HostJobLifetime.h'), (Join-Path $hostRoot 'OptiScaler\dlssnr\backend\HostJobLifetime.h'), $true)
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.h' '#include "LmxxfEvaluateCut.h"' "#include `"LmxxfEvaluateCut.h`"`n#include `"HostJobLifetime.h`""
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.h' '        ID3D12CommandList* cmd = nullptr;' "        ID3D12CommandList* cmd = nullptr;`n        HostJobLifetime lifetime {};"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfBackend.h' '    bool sessionUnavailable = false;' '    std::atomic<bool> sessionUnavailable { false };'
Replace-Section 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' 'ID3D12Resource* LmxxfBackend::FinishRecord(' "namespace`n{" 'finish_record.inc'
Replace-Section 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' 'void LmxxfBackend::Submitted(' '// Any thread, from inside' 'submitted.inc'
Replace-Section 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' 'void LmxxfBackend::ListRecycled(' '// Stops OnListRecycled' 'list_recycled.inc'
Replace-Section 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' 'void LmxxfBackend::MigrateQueue(' 'bool LmxxfBackend::Shutdown(' 'migrate_queue.inc'
Replace-Section 'OptiScaler\dlssnr\backend\LmxxfBackend.cpp' 'bool LmxxfBackend::Shutdown(' '// The exit hook runs' 'shutdown.inc'
Replace-Once 'OptiScaler\dlssnr\submission\SubmissionTls.h' 'inline thread_local int g_logicalExecuteDepth = 0;' "inline thread_local int g_logicalExecuteDepth = 0;`n// Failure-only: the between callback could not establish safe output ordering.`ninline thread_local bool g_abortContinuation = false;"
Replace-Once 'OptiScaler\dlssnr\submission\LogicalList.h' "        if (split && between)`n            between(queue, betweenCtx);`n        if (split && continuation)" @'
        bool abortContinuation = false;
        if (split && between)
        {
            const bool previousAbort = g_abortContinuation;
            g_abortContinuation = false;
            between(queue, betweenCtx);
            abortContinuation = g_abortContinuation;
            g_abortContinuation = previousAbort;
        }
        if (split && continuation && !abortContinuation)
'@
Replace-Once 'OptiScaler\dlssnr\submission\LogicalList.h' "        if (FAILED(hr))`n            return hr;`n        return S_OK;`n    }`n`n    HRESULT Reset" "        if (FAILED(hr))`n            return hr;`n        return abortContinuation ? E_FAIL : S_OK;`n    }`n`n    HRESULT Reset"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' 'using GetLastErrorFn = int32_t (*)(char* buffer, uint32_t buffer_chars);' "using GetLastErrorFn = int32_t (*)(char* buffer, uint32_t buffer_chars);`nusing RecoverSubmissionFn = int32_t (*)(void* session, void* job, void* actualQueue);`nconstexpr int32_t kRecordingFailed = static_cast<int32_t>(0x52454346);"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' '    GetLastErrorFn getLastError = nullptr;' "    GetLastErrorFn getLastError = nullptr;`n    RecoverSubmissionFn recoverSubmission = nullptr;`n    bool requireSafeContinuation = false, recoveryOnly = false;"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' '    ID3D12CommandQueue* lastEnqueueQueue = nullptr;' "    ID3D12CommandQueue* lastEnqueueQueue = nullptr;`n    void* lastSession = nullptr;`n    void* lastJob = nullptr;`n    bool lastRecovered = false, lastUnsafeContinuation = false;`n    int32_t lastRecoveryRc = -1;"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' '    p.getLastError = nullptr;' "    p.getLastError = nullptr;`n    p.recoverSubmission = nullptr;`n    p.requireSafeContinuation = false; p.recoveryOnly = false;"
Replace-Section 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' 'inline void BetweenThunk(' '// QI for ILogicalCommandList' 'between_thunk.inc'
Replace-Section 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' 'inline void SetPendingEnqueue(' 'struct EnqueueDiagnostic' 'set_pending.inc'
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' "    ID3D12CommandQueue* queue = nullptr;`n};" "    ID3D12CommandQueue* queue = nullptr;`n    void* session = nullptr;`n    void* job = nullptr;`n    bool recovered = false;`n    bool unsafeContinuation = false;`n    int32_t recoveryRc = -1;`n};"
Replace-Once 'OptiScaler\dlssnr\backend\LmxxfEvaluateCut.h' '    return { p.lastEnqueueRc.load(std::memory_order_relaxed), p.lastEnqueueError, p.lastEnqueueQueue };' "    return { p.lastEnqueueRc.load(std::memory_order_relaxed), p.lastEnqueueError, p.lastEnqueueQueue,`n             p.lastSession, p.lastJob, p.lastRecovered, p.lastUnsafeContinuation, p.lastRecoveryRc };"
Write-Host 'Applied pinned OpenDLSS AMD host integration.'
$menuPath = Join-Path $hostRoot 'OptiScaler\dlssnr\DlssNr_Menu.cpp'
$menu = [IO.File]::ReadAllText($menuPath).Replace("`r`n","`n")
if (-not $menu.Contains('OpenDLSS-NR AMD (development)')) {
  $startToken = "        if (DlssNr::AmdBridge::HasFiles() &&`n            DlssNr::Backend::ActiveKindFromConfig() == DlssNr::Backend::Kind::Mochizuki)"
  $endToken = "        if (DlssNr::AmdBridge::HasFiles())`n        {`n            // Runtime name belongs"
  $start = $menu.IndexOf($startToken)
  $end = $menu.IndexOf($endToken,$start)
  if ($start -lt 0 -or $end -lt 0) { throw 'Pinned NR menu block did not match' }
  $panel = [IO.File]::ReadAllText((Join-Path $root 'integrations\optiscaler\menu.inc'))
  $menu = $menu.Substring(0,$start) + $panel + $menu.Substring($end)
  $menu = $menu.Replace('{ Kind::Mochizuki, "mochizuki", "mochizuki",','{ Kind::Mochizuki, "mochizuki", "OpenDLSS AMD",')
  [IO.File]::WriteAllText($menuPath,$menu,[Text.UTF8Encoding]::new($false))
}
Replace-Once 'OptiScaler\dlssnr\DlssNr_Menu.cpp' "        if (ImGui::Checkbox(`"Enable NR`", &enabled))`n            config->DlssNrEnabled = enabled;" @'
        if (ImGui::Checkbox("Enable NR", &enabled))
        {
            config->DlssNrEnabled = enabled;
            DlssNr::AmdBridge::InvalidateHistory();
        }
'@
Replace-Once 'OptiScaler\menu\menu_common.cpp' '            config->DlssNrEnabled = !config->DlssNrEnabled.value_or_default();' "            config->DlssNrEnabled = !config->DlssNrEnabled.value_or_default();`n            DlssNr::AmdBridge::InvalidateHistory();"
Replace-Once 'OptiScaler\menu\menu_common.cpp' '#include <dlssnr/DlssNr.h>' "#include <dlssnr/DlssNr.h>`n#include <dlssnr/amd/AmdBridge.h>"
$recoveryTests = [IO.File]::ReadAllText((Join-Path $root 'integrations\optiscaler\recovery_tests.inc'))
if ([IO.File]::ReadAllText((Join-Path $hostRoot 'tests\lmxxf_evaluate_cut.cpp')).Contains('static int g_recoveryCalls = 0;')) {
  Replace-Section 'tests\lmxxf_evaluate_cut.cpp' 'static int g_recoveryCalls = 0;' 'int main()' 'recovery_tests.inc'
} else {
  Replace-Once 'tests\lmxxf_evaluate_cut.cpp' 'int main()' ($recoveryTests + "`nint main()")
}
Replace-Once 'tests\lmxxf_evaluate_cut.cpp' '    // Exercise the same queue drain and awaiting-list implementations used by the host.' "    RunNativeRecoveryTests(device, queue);`n    // Exercise the same queue drain and awaiting-list implementations used by the host."
