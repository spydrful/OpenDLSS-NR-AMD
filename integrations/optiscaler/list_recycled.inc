// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the pinned OptiScaler host's LmxxfBackend::ListRecycled.
void LmxxfBackend::ListRecycled(ID3D12CommandList* list)
{
    if (!list || pendingList.load(std::memory_order_acquire) != list)
        return;
    std::lock_guard lock(jobMutex);
    if (exiting.load(std::memory_order_acquire) || !pendingJobInfo.job || pendingJobInfo.cmd != list)
        return;
    void* job = pendingJobInfo.job;
    LmxxfCut::ClearPendingEnqueueIf(list, job);
    pendingJobInfo.cmd = nullptr;
    pendingList.store(nullptr, std::memory_order_release);
    const bool cancelled = pendingJobInfo.lifetime.CancelWithProof(true, false, [&] {
        return session && api && api->table.CancelUnsubmitted
                   ? api->table.CancelUnsubmitted(session, job) : LMXXF_NR_FAILED;
    });
    if (!cancelled)
    {
        pendingJobInfo.lifetime.quarantined = true;
        sessionUnavailable = true;
        SetStatus("OpenDLSS AMD: recycled submitted job quarantined; graphics restart required");
        return;
    }
    if (api->table.ResetHistory) api->table.ResetHistory(session);
    pendingJobInfo = {};
    SetStatus("lmxxf: discarded recording cancelled; history reset");
}
