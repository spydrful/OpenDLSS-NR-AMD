// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the pinned OptiScaler host's LmxxfBackend::FinishRecord.
ID3D12Resource* LmxxfBackend::FinishRecord(ID3D12GraphicsCommandList* recordCmd, void* jobHandle, void* privateOutput)
{
    // Publish ownership before the first command can reference a job resource.
    // A failed recording remains live until discard or actual GPU completion.
    {
        std::lock_guard lock(jobMutex);
        pendingJobInfo = {};
        pendingJobInfo.job = jobHandle;
        pendingJobInfo.cmd = recordCmd;
        pendingList.store(recordCmd, std::memory_order_release);
    }
    DlssNr::Submission::ILogicalCommandList* runtimeLogical = nullptr;
    if (api->setFrameMetadata)
        recordCmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList), reinterpret_cast<void**>(&runtimeLogical));
    auto runtimeList = runtimeLogical ? runtimeLogical->RuntimeSegment() : recordCmd;
    const int32_t inputRc = api->table.RecordInputs(session, jobHandle, runtimeList);
    if (runtimeLogical) { runtimeLogical->RestoreRuntimeBindings(); runtimeLogical->Release(); runtimeLogical = nullptr; }
    const HRESULT splitHr = LmxxfCut::TrySplitAtEvaluate(recordCmd);
    const bool split = splitHr == S_OK;
    int32_t outputRc = LMXXF_NR_OK;
    if (inputRc == LMXXF_NR_OK && split)
    {
        if (api->setFrameMetadata)
            recordCmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList), reinterpret_cast<void**>(&runtimeLogical));
        runtimeList = runtimeLogical ? runtimeLogical->RuntimeSegment() : recordCmd;
        outputRc = api->table.RecordOutputs(session, jobHandle, runtimeList);
        if (runtimeLogical) { runtimeLogical->RestoreRuntimeBindings(); runtimeLogical->Release(); }
    }
    const bool failed = inputRc != LMXXF_NR_OK || !split || outputRc != LMXXF_NR_OK;
    {
        std::lock_guard lock(jobMutex);
        pendingJobInfo.lifetime.recordingFailed = failed;
    }
    if (split)
    {
        // A failed native recording runs only the isolated original-color
        // recovery. A runtime lacking that export is quarantined fail-closed.
        LmxxfCut::SetPendingEnqueue(session, jobHandle, api->table.EnqueueHip, api->table.GetLastError,
                                   recordCmd, queue, api->recoverSubmission, api->setFrameMetadata != nullptr, failed);
        LmxxfCut::ArmBetweenSlot();
    }
    if (failed)
    {
        SetStatus("OpenDLSS AMD: recording failed; job retained until discard or safe recovery");
        return nullptr;
    }
    SetStatus("lmxxf: Record ok (pending EnqueueHip)");
    return reinterpret_cast<ID3D12Resource*>(privateOutput);
}
