// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the pinned OptiScaler host's LmxxfBackend::Shutdown.
bool LmxxfBackend::Shutdown()
{
    LmxxfCut::DisarmBetweenSlot();
    bool liveJob = false;
    {
        std::lock_guard lock(jobMutex);
        liveJob = pendingJobInfo.job != nullptr || cancelling;
        pendingList.store(nullptr, std::memory_order_release);
        if (migrateTo) { migrateTo->Release(); migrateTo = nullptr; }
    }
    // A pending recording still owns commands into the runtime's buffers.
    // Without discard/completion proof, leave both session and DLL mapped.
    const bool destroyed = !liveJob && (!session ||
        (api && api->table.Destroy && api->table.Destroy(session) == LMXXF_NR_OK));
    if (!destroyed)
    {
        sessionUnavailable = true;
        sessionReady = false;
        SetStatus("OpenDLSS AMD: live session/DLL retained for GPU safety");
        return false;
    }
    session = nullptr;
    sessionReady = false;
    ClearRuntimeStatus();
    if (runtimeDll) { FreeLibrary(reinterpret_cast<HMODULE>(runtimeDll)); runtimeDll = nullptr; }
    if (api)
    {
        api->table = {};
        api->setFrameMetadata = nullptr;
        api->recoverSubmission = nullptr;
        api->getFeatures = nullptr;
        api->setControls = nullptr;
        api->getInfo = nullptr;
        api->getControlDefaults = nullptr;
        api->setExposure = nullptr;
        api->anyQueue = false;
        api->controlsSentValid = false;
        if (api->exposureRing) { api->exposureRing->Release(); api->exposureRing = nullptr; api->exposureMapped = nullptr; }
        api->exposureFrames = 0;
    }
    SetStatus("lmxxf: shutdown");
    return true;
}
