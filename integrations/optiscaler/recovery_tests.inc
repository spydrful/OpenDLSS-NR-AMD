// SPDX-License-Identifier: GPL-3.0-or-later
// D3D12 submission-cut recovery tests for the GPL OptiScaler adapter.
static int g_recoveryCalls = 0;
static int32_t RecoverOk(void* session, void* job, void* queue)
{
    Require(session == reinterpret_cast<void*>(0x1111) && job == reinterpret_cast<void*>(0x2222) && queue,
            "recovery receives the actual pending session/job/queue");
    ++g_recoveryCalls;
    return 0;
}
static int32_t RecoverFailed(void*, void*, void*) { ++g_recoveryCalls; return 5; }

static ID3D12Resource* RecoveryBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {}; heap.Type = type;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)), "recovery test buffer");
    return resource;
}
static void RunNativeRecoveryTests(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    using namespace DlssNr::Backend::LmxxfCut;
    struct Case { bool enqueueFails, recoveryFails, missingRecovery, recordingFails, continuation; };
    const Case cases[] = {
        {false, false, false, false, true}, // normal: recovery never called
        {true, false, false, false, true},  // safe marker fallback
        {true, true, false, false, false},  // uncertain Vulkan work: suppress consumer
        {true, false, true, false, false},  // older native DLL: fail closed
        {false, false, false, true, true}   // partial recording: recovery only
    };
    for (const auto& test : cases)
    {
        ID3D12CommandAllocator* alloc = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "recovery alloc");
        ID3D12GraphicsCommandList* list = nullptr;
        Check(DlssNr::Submission::Hooks::CreateProxiedCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
              alloc, nullptr, IID_PPV_ARGS(&list)), "recovery proxy");
        auto source = RecoveryBuffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto result = RecoveryBuffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        uint32_t* mapped = nullptr;
        Check(source->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "recovery upload map");
        *mapped = 0x12345678; source->Unmap(0, nullptr);
        Check(result->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "recovery readback init");
        *mapped = 0; result->Unmap(0, nullptr);
        Require(TrySplitAtEvaluate(list) == S_OK, "recovery split");
        list->CopyBufferRegion(result, 0, source, 0, 4);
        Check(list->Close(), "recovery close");
        const int beforeRecovery = g_recoveryCalls;
        const int beforeEnqueue = Pending().enqueueCalls.load();
        const uint64_t beforeContinuation = DlssNr::Submission::g_continuationSubmissions.load();
        const uint64_t beforeFailure = DlssNr::Submission::g_submissionFailures.load();
        SetPendingEnqueue(reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222),
                          test.enqueueFails ? &FakeEnqueueFailure : &FakeEnqueue, &FakeLastError,
                          list, queue, test.missingRecovery ? nullptr : test.recoveryFails ? &RecoverFailed : &RecoverOk,
                          true, test.recordingFails);
        ArmBetweenSlot();
        auto between = DlssNr::Submission::Hooks::GetBetween();
        ID3D12CommandList* batch[] = {list};
        DlssNr::Submission::Hooks::ExecuteExpanded(queue, 1, batch, between.fn, between.ctx,
            [](ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c) { q->ExecuteCommandLists(n, c); });
        Require(DlssNr::Backend::DrainQueue(device, queue, 1000), "recovery actual queue drain");
        Check(result->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "recovery result map");
        Require(*mapped == (test.continuation ? 0x12345678u : 0u),
                "real continuation copy executes only when safe");
        result->Unmap(0, nullptr);
        const auto diagnostic = LastEnqueueDiagnostic();
        const bool needsRecovery = test.enqueueFails || test.recordingFails;
        Require(g_recoveryCalls - beforeRecovery == int(needsRecovery && !test.missingRecovery),
                "normal success never invokes failure recovery");
        Require(Pending().enqueueCalls.load() - beforeEnqueue == int(!test.recordingFails),
                "partial recording skips neural enqueue");
        Require(diagnostic.recovered == (needsRecovery && test.continuation) &&
                diagnostic.unsafeContinuation == !test.continuation,
                "recovery diagnostic retains safe/unsafe credential");
        Require(DlssNr::Submission::g_continuationSubmissions.load() - beforeContinuation == uint64_t(test.continuation),
                "consumer submit count matches actual execution");
        Require(DlssNr::Submission::g_submissionFailures.load() - beforeFailure == uint64_t(!test.continuation),
                "unsafe consumer is reported as failed submission");
        if (test.enqueueFails)
            Require(diagnostic.rc == 5, "fallback preserves the original Enqueue failure");
        ClearPendingEnqueueIfSubmitted(1, batch);
        list->Release(); alloc->Release(); source->Release(); result->Release();
    }
    std::puts("native host recovery: PASS (normal, recovered, failed/missing recovery, partial recording)");
}
