#pragma once

/* Retained versioned compatibility C ABI, implemented by OpenNrRuntime.dll.
 * Hosts and runtimes must not share a C++ ABI. No STL, exceptions, or
 * CRT-allocated objects cross this boundary. x64 stdcall is the Windows default. */

#include <stdint.h>
#include <stddef.h> /* wchar_t in C hosts */

#ifdef __cplusplus
extern "C"
{
#endif

#define LMXXF_NR_ABI_VERSION 1u

/* Retained compatibility flag accepted by OpenNR. Its private GPU marker selects
 * original scene color when inference fails before Vulkan submission; it does
 * not mean that the private output is fully zeroed. Normal EnqueueHip requires
 * the session's creation queue and rejects queue changes before fence signals.
 * The optional OpenNrRecoverSubmission export establishes failure-only fallback
 * after prior work completes. Successful enqueue or recovery still requires the
 * caller to submit its consumer before Retire, Drain, or Destroy. See game/README.md
 * for the producer/consumer boundary and failure-lifetime requirements. */
#define LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK (1u << 0)

    enum LmxxfNrStatus
    {
        LMXXF_NR_OK = 0,
        LMXXF_NR_UNSUPPORTED_ABI = 1,
        LMXXF_NR_INVALID_ARGUMENT = 2,
        LMXXF_NR_NOT_IMPLEMENTED = 3,
        LMXXF_NR_UNAVAILABLE = 4,
        LMXXF_NR_FAILED = 5
    };

    enum LmxxfNrJobState
    {
        LMXXF_NR_JOB_NONE = 0,
        LMXXF_NR_JOB_PREPARED = 1,           /* Set by PrepareFrame; ready for RecordInputs */
        LMXXF_NR_JOB_PRODUCER_SUBMITTED = 2, /* Set by RecordInputs; producer recorded/submitted */
        LMXXF_NR_JOB_NR_ENQUEUED = 3,        /* Vulkan inference or recovered fallback scheduled */
        LMXXF_NR_JOB_NR_COMPLETE = 4,        /* Vulkan inference completed */
        LMXXF_NR_JOB_CONSUMER_COMPLETE = 5,  /* Set by RecordOutputs; consumer recorded */
        LMXXF_NR_JOB_RETIRED = 6             /* Set by Retire; canceled unsubmitted jobs become NONE */
    };

    typedef struct LmxxfNrCapabilities
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint32_t max_input_width;
        uint32_t max_input_height;
        uint32_t history_supported; /* OpenNR: 1, motion-reprojected history is implemented */
        uint32_t overlap_supported; /* OpenNR: 0, shared neural work is ordered on one queue */
        uint32_t graph_supported;   /* OpenNR: 0, legacy HIP graph capability is not used */
        uint32_t hip_ready;         /* Legacy name: OpenNR Vulkan compatibility implementation is present */
        uint32_t gfx1201_target;    /* Legacy RX 9070 XT target marker; query OpenNR device capabilities */
    } LmxxfNrCapabilities;

    typedef struct LmxxfNrCreateInfo
    {
        uint32_t struct_size;
        void* device; /* ID3D12Device*; retained and checked by Create */
        void* queue;  /* ID3D12CommandQueue*; same-device DIRECT queue required */
        const wchar_t* assets_directory;
        uint32_t flags; /* LMXXF_NR_CREATE_FLAG_*; unknown bits are rejected */
    } LmxxfNrCreateInfo;

#define LMXXF_NR_FRAME_FLAG_STRENGTH (1u << 0)
#define LMXXF_NR_FRAME_FLAG_DEBUG_VIEW (1u << 1)
#define LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH (1u << 2)
/* Each pass reads its own output from the previous frame, warped by the motion fields below. */
#define LMXXF_NR_FRAME_FLAG_TEMPORAL (1u << 3)
/* With TEMPORAL: the smoothing below acts on the network's change to the frame, not on its whole output. */
#define LMXXF_NR_FRAME_FLAG_SMOOTH_RESIDUAL (1u << 4)

    typedef struct LmxxfNrFrameInfo
    {
        uint32_t struct_size;
        uint64_t session_id;
        uint64_t frame_id;
        uint64_t list_generation;
        void* command_list; /* ID3D12GraphicsCommandList*; Record* do not Execute */
        uint32_t color_width;
        uint32_t color_height;
        void* color;             /* ID3D12Resource*; required for RecordInputs */
        uint32_t color_state;    /* D3D12_RESOURCE_STATES at RecordInputs */
        uint32_t flags;          /* OpenNR accepts STRENGTH and TEMPORAL; other bits are declined */
        float transfer_strength; /* OpenNR requires 1.0 when STRENGTH is requested */
        float color_strength;    /* Colour strength: lmxxf 0..2, mochizuki 0..4, default 1.0; above 1 extrapolates */
        uint32_t debug_view;     /* OpenNR supports 0 only; compatibility debug modes are declined */
        float model_scale;       /* OpenNR requires full render resolution: 1.0, or legacy default 0 */
        uint32_t passes;         /* OpenNR supports one pass; 0 is the legacy default for one */
        void* motion;            /* ID3D12Resource*, the game's motion vectors; required for TEMPORAL */
        uint32_t motion_state;   /* D3D12_RESOURCE_STATES at RecordInputs */
        uint32_t motion_width;   /* extent the vectors cover; 0 = color_width x color_height */
        uint32_t motion_height;
        float motion_scale_x; /* value * scale = pixels of that extent (the NGX and FFX convention) */
        float motion_scale_y;
        uint32_t reset; /* nonzero drops every pass's history this frame */
        /* TEMPORAL: where the last pass's output differs from its warped history by less than the threshold
           (colour units, 0..1), it is pulled toward the history, by the strength at no difference. 0 = off.
           SMOOTH_RESIDUAL applies the same rule to the output minus the frame's input, against that difference
           from the previous frame, and adds the result back to the input. */
        float smooth_threshold;
        float smooth_strength;
    } LmxxfNrFrameInfo;

    typedef struct LmxxfNrJob
    {
        uint32_t struct_size;
        void* handle;
        void* private_output; /* ID3D12Resource* for SR; null until PrepareFrame succeeds */
    } LmxxfNrJob;

    typedef struct LmxxfNrApi
    {
        uint32_t struct_size;
        uint32_t abi_version;
        int32_t (*QueryCapabilities)(LmxxfNrCapabilities* out);
        int32_t (*Create)(const LmxxfNrCreateInfo* info, void** context);
        int32_t (*Destroy)(void* context);
        int32_t (*PrepareSession)(void* context);
        int32_t (*PrepareFrame)(void* context, const LmxxfNrFrameInfo* info, LmxxfNrJob* job);
        int32_t (*RecordInputs)(void* context, void* job, void* command_list);
        int32_t (*EnqueueHip)(void* context, void* job, void* command_queue); /* OpenNR spelling of Vulkan Enqueue */
        int32_t (*RecordOutputs)(void* context, void* job, void* command_list);
        int32_t (*ExecuteAfterProducer)(void* context, void* job, void* command_queue);
        int32_t (*CancelUnsubmitted)(void* context, void* job);
        int32_t (*Poll)(void* context, void* job, uint32_t* state);
        int32_t (*Retire)(void* context, void* job);
        int32_t (*ResetHistory)(void* context);
        int32_t (*Drain)(void* context);
        int32_t (*GetStatus)(void* context, char* buf, uint32_t buf_chars);
        int32_t (*GetLastError)(char* buf, uint32_t buf_chars);
    } LmxxfNrApi;

#ifdef _WIN32
#ifdef LMXXF_NR_RUNTIME_EXPORTS
#define LMXXF_NR_EXPORT __declspec(dllexport)
#else
#define LMXXF_NR_EXPORT __declspec(dllimport)
#endif
#else
#define LMXXF_NR_EXPORT
#endif

    /* Compatibility table export. Caller sets out->struct_size = sizeof(LmxxfNrApi).
     * OpenNR also provides OpenNr* and optional MochizukiNr* exports. */
    LMXXF_NR_EXPORT int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi* out);

#ifdef __cplusplus
}
#endif
