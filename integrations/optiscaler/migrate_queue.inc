// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the pinned OptiScaler host's LmxxfBackend::MigrateQueue.
void LmxxfBackend::MigrateQueue(ID3D12CommandQueue* q)
{
    LmxxfCut::ClearPendingEnqueue();
    const bool actualDrained = DrainQueue(device, q);
    const bool sessionDrained = !session || (api && api->table.Drain && api->table.Drain(session) == LMXXF_NR_OK);
    const bool destroyed = actualDrained && sessionDrained &&
        (!session || (api && api->table.Destroy && api->table.Destroy(session) == LMXXF_NR_OK));
    if (!destroyed)
    {
        // Keep the runtime module and the old session's resources mapped.
        // q owns the migration reference, which is no longer needed.
        q->Release();
        sessionUnavailable = true;
        sessionReady = false;
        SetStatus("OpenDLSS AMD: session drain/destroy failed; graphics restart required");
        return;
    }
    session = nullptr;
    sessionReady = false;
    ClearRuntimeStatus();
    ID3D12CommandQueue* previous = nullptr;
    {
        std::lock_guard lock(jobMutex);
        previous = queue;
        queue = q;
    }
    if (previous) previous->Release();
    DlssNr::AmdBridge::UpdateConfirmedRenderQueue(q);
    SetStatus("lmxxf: session safely recreated on actual render queue");
}
