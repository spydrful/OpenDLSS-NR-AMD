// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the pinned OptiScaler host's LmxxfBackend::Submitted.
void LmxxfBackend::Submitted(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
{
    if (exiting.load(std::memory_order_acquire))
        return;
    std::lock_guard lock(jobMutex);
    bool contains = false;
    for (UINT i = 0; lists && i < count; ++i)
        contains |= pendingJobInfo.cmd && lists[i] == pendingJobInfo.cmd;
    if (!contains || !pendingJobInfo.job || !session || !api)
        return;

    auto& lifetime = pendingJobInfo.lifetime;
    lifetime.submitted = true;
    const auto diagnostic = LmxxfCut::LastEnqueueDiagnostic();
    const bool betweenRan = diagnostic.job == pendingJobInfo.job && diagnostic.session == session;
    const bool native = api->setFrameMetadata != nullptr;
    const bool failedBetween = betweenRan && diagnostic.rc != LMXXF_NR_OK;
    bool released = false;
    bool recreate = false;
    if (betweenRan && diagnostic.unsafeContinuation)
    {
        // Vulkan may have been submitted. Cancel is never valid here, even
        // after the D3D queue drains. Keep the session and DLL mapped.
        lifetime.quarantined = true;
    }
    else if (lifetime.recordingFailed && !betweenRan)
    {
        // Split failed, so no between callback can run. The original game
        // list may still contain pack commands: wait its actual queue first.
        const bool completed = DrainQueue(device, q);
        released = lifetime.CancelWithProof(false, completed, [&] {
            return api->table.CancelUnsubmitted(session, pendingJobInfo.job);
        });
        if (!released)
            lifetime.quarantined = true;
        recreate = native;
    }
    else
    {
        const int32_t rc = api->table.Retire ? api->table.Retire(session, pendingJobInfo.job) : LMXXF_NR_FAILED;
        released = rc == LMXXF_NR_OK;
        if (!released)
        {
            // Retire failed after submission. Preserve ownership; no blind
            // cancel or reuse, including on a later command-list Reset.
            lifetime.quarantined = true;
            char error[256] {};
            if (api->table.GetLastError) api->table.GetLastError(error, sizeof(error));
            LOG_ERROR("OpenDLSS AMD: Retire failed rc={} err={}; job quarantined", rc, error);
        }
        recreate = native && (lifetime.recordingFailed || failedBetween);
    }
    LmxxfCut::ClearPendingEnqueueIfSubmitted(count, lists);
    if (!released)
    {
        sessionUnavailable = true;
        SetStatus("OpenDLSS AMD: unsafe job quarantined; graphics restart required");
        return;
    }
    pendingJobInfo = {};
    pendingList.store(nullptr, std::memory_order_release);
    if (api->table.ResetHistory && (recreate || failedBetween))
        api->table.ResetHistory(session);
    if (q && (recreate || (!SameObject(q, queue) && !api->anyQueue)))
    {
        // Render-thread recreation drains the actual submitted queue and the
        // old session. Recovery also needs recreation on an unchanged queue.
        q->AddRef();
        if (migrateTo) migrateTo->Release();
        migrateTo = q;
        SetStatus("OpenDLSS AMD: original-color fallback; session recreation pending");
    }
}
