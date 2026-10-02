// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the pinned OptiScaler host's evaluate-cut submission callback.
inline void BetweenThunk(ID3D12CommandQueue* queue, ID3D12CommandList* list, void* /*ctx*/)
{
    if (g_exiting.load(std::memory_order_acquire)) return;
    auto& p = Pending();
    void* session = nullptr;
    void* job = nullptr;
    EnqueueHipFn enqueue = nullptr;
    RecoverSubmissionFn recover = nullptr;
    GetLastErrorFn lastError = nullptr;
    bool requireSafe = false, recoveryOnly = false, match = true;
    {
        std::lock_guard lock(p.mutex);
        if (!p.targetList || p.targetList != list) return;
        session = p.session; job = p.job; enqueue = p.enqueueHip;
        recover = p.recoverSubmission; lastError = p.getLastError;
        requireSafe = p.requireSafeContinuation; recoveryOnly = p.recoveryOnly;
        if (p.expectedQueue && queue)
        {
            IUnknown* a = nullptr; IUnknown* b = nullptr;
            queue->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&a));
            p.expectedQueue->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&b));
            match = a && b && a == b;
            if (a) a->Release(); if (b) b->Release();
        }
        p.session = nullptr; p.job = nullptr; p.enqueueHip = nullptr;
        p.getLastError = nullptr; p.recoverSubmission = nullptr;
        p.targetList = nullptr; p.expectedQueue = nullptr;
        p.requireSafeContinuation = false; p.recoveryOnly = false;
    }
    p.betweenHits.fetch_add(1, std::memory_order_relaxed);
    if (!match) p.skippedHits.fetch_add(1, std::memory_order_relaxed);
    int32_t rc = kEnqueueSkipped;
    if (enqueue && session && job && !recoveryOnly)
    {
        p.enqueueCalls.fetch_add(1, std::memory_order_relaxed);
        rc = enqueue(session, job, queue);
    }
    else if (recoveryOnly)
        rc = kRecordingFailed;
    else
        p.skippedHits.fetch_add(1, std::memory_order_relaxed);
    std::array<char, 256> error {};
    if (lastError) lastError(error.data(), static_cast<uint32_t>(error.size()));
    error.back() = 0;
    bool recovered = false;
    const bool needsRecovery = requireSafe && rc != 0;
    int32_t recoveryRc = -1;
    if (needsRecovery && recover && session && job)
        recovered = (recoveryRc = recover(session, job, queue)) == 0;
    // A failed Vulkan submission can still have GPU work in flight. The
    // continuation must not touch its output unless recovery established the
    // original-color marker and actual-queue ordering.
    if (needsRecovery && !recovered)
        DlssNr::Submission::g_abortContinuation = true;
    {
        std::lock_guard lock(p.mutex);
        p.lastEnqueueRc.store(!match && rc == 0 ? kEnqueueQueueMismatch : rc, std::memory_order_relaxed);
        p.lastEnqueueError = error;
        p.lastEnqueueQueue = queue;
        p.lastSession = session; p.lastJob = job;
        p.lastRecovered = recovered;
        p.lastRecoveryRc = recoveryRc;
        p.lastUnsafeContinuation = needsRecovery && !recovered;
    }
}
