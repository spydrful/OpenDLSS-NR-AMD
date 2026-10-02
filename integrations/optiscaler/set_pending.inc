// SPDX-License-Identifier: GPL-3.0-or-later
inline void SetPendingEnqueue(void* session, void* job, EnqueueHipFn enqueueHip, GetLastErrorFn getLastError,
                              ID3D12CommandList* targetList, ID3D12CommandQueue* expectedQueue = nullptr,
                              RecoverSubmissionFn recover = nullptr, bool requireSafeContinuation = false,
                              bool recoveryOnly = false)
{
    auto& p = Pending();
    std::lock_guard lock(p.mutex);
    p.session = session; p.job = job; p.enqueueHip = enqueueHip;
    p.getLastError = getLastError; p.targetList = targetList; p.expectedQueue = expectedQueue;
    p.recoverSubmission = recover;
    p.requireSafeContinuation = requireSafeContinuation;
    p.recoveryOnly = recoveryOnly;
    // Invalidate the old diagnostic before a pointer can be reused for a new
    // prepared job. Submitted only consumes a matching session/job credential.
    p.lastSession = nullptr; p.lastJob = nullptr; p.lastEnqueueQueue = nullptr;
    p.lastRecovered = false; p.lastUnsafeContinuation = false;
}
