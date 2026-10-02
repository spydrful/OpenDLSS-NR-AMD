// SPDX-License-Identifier: GPL-3.0-or-later
// OpenDLSS-NR-AMD integration with the pinned OptiScaler GPL host.
#pragma once
#include <cstdint>
#include "lmxxf_runtime/LmxxfNrApi.h"

namespace DlssNr::Backend
{
inline uint32_t HostFrameFlags(uint32_t requested, bool openNr)
{
    // DEBUG_VIEW is a legacy runtime mode, even when debug_view itself is zero.
    // Preserve diagnostic passthrough requests so unsupported modes fail
    // explicitly rather than silently running the neural model.
    return openNr ? requested & ~LMXXF_NR_FRAME_FLAG_DEBUG_VIEW : requested;
}
struct HostJobLifetime
{
    bool recordingFailed = false;
    bool submitted = false;
    bool quarantined = false;

    // Cancellation is permitted only after the recording was discarded, or
    // after a failed, never-enqueued recording's actual queue completed.
    template<class Cancel>
    bool CancelWithProof(bool discarded, bool actualQueueCompleted, Cancel&& cancel)
    {
        if (quarantined || (submitted ? !(recordingFailed && actualQueueCompleted) : !discarded))
            return false;
        if (cancel() != 0)
        {
            quarantined = true;
            return false;
        }
        return true;
    }
};
} // namespace DlssNr::Backend
