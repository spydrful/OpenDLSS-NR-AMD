#include "../integrations/optiscaler/HostJobLifetime.h"
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main()
{
    using DlssNr::Backend::HostJobLifetime;
    using DlssNr::Backend::HostFrameFlags;
    const uint32_t legacy = LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW | LMXXF_NR_FRAME_FLAG_TEMPORAL;
    require(HostFrameFlags(legacy, true) == (LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_TEMPORAL),
            "native frame flags do not request an unavailable debug mode");
    require(HostFrameFlags(legacy, false) == legacy, "legacy runtime flags remain unchanged");
    require(HostFrameFlags(legacy | LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH, true) & LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH,
            "unsupported diagnostic passthrough cannot silently run the model");
    int cancels = 0;
    auto cancel = [&] { ++cancels; return 0; };
    HostJobLifetime recorded;
    require(!recorded.CancelWithProof(false, false, cancel) && cancels == 0,
            "recorded commands retain resources until a discard/completion credential");
    require(recorded.CancelWithProof(true, false, cancel) && cancels == 1,
            "never-submitted discarded list can cancel");
    HostJobLifetime submitted;
    submitted.submitted = true;
    require(!submitted.CancelWithProof(true, false, cancel) && cancels == 1,
            "Reset is not GPU completion after submission");
    submitted.recordingFailed = true;
    require(!submitted.CancelWithProof(false, false, cancel) && cancels == 1,
            "failed recording cannot cancel while actual queue is live");
    require(submitted.CancelWithProof(false, true, cancel) && cancels == 2,
            "failed never-enqueued recording can cancel after actual queue completion");
    HostJobLifetime uncertain;
    require(!uncertain.CancelWithProof(true, false, [&] { ++cancels; return 5; }),
            "runtime cancellation failure retains ownership");
    require(uncertain.quarantined && !uncertain.CancelWithProof(true, true, cancel) && cancels == 3,
            "quarantined job cannot be cancelled again or reused");
    HostJobLifetime enqueued;
    enqueued.submitted = true;
    require(!enqueued.CancelWithProof(false, true, cancel) && cancels == 3,
            "ordinary enqueued job always uses Retire, even after D3D completion");
    std::puts("host_job_lifetime_tests: PASS (11 checks)");
}
