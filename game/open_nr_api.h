// OpenDLSS-NR-AMD game interface. POD only: no C++ allocation or exception crosses the DLL boundary.
#pragma once
#include <stdint.h>
#include "compat/LmxxfNrApi.h"
#define OPEN_NR_ABI_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OpenNrFrameMetadata {
  uint32_t struct_size, abi_version;
  void* exposure;
  uint32_t exposure_state;
  float pre_exposure, exposure_scale;
  uint32_t color_x, color_y, motion_x, motion_y;
  float jitter_x, jitter_y; // current render-pixel jitter; zero when motion includes jitter
} OpenNrFrameMetadata;
typedef struct OpenNrDeviceCapabilities {
  uint32_t struct_size, abi_version, supported;
  uint32_t vendor_id, device_id, wave_size, matrix_m, matrix_n, matrix_k;
  uint32_t fp8_e4m3, accumulator_fp32, shared_buffers, shared_timeline_fences;
  uint32_t max_input_width, max_input_height;
  uint64_t device_local_bytes, max_storage_buffer_bytes;
  char device_name[256], driver[256], arithmetic_mode[64];
} OpenNrDeviceCapabilities;
typedef struct OpenNrTimings {
  uint32_t struct_size, abi_version;
  uint64_t frame_id, submitted_frames, bypassed_frames, allocated_neural_bytes;
  double pack_ms, preprocess_ms, inference_ms, composite_ms, unpack_ms, nr_bridge_ms;
} OpenNrTimings;
typedef struct OpenNrApi {
  uint32_t struct_size, abi_version;
  LmxxfNrApi lifecycle; // EnqueueHip is the compatibility spelling of Vulkan Enqueue.
  int32_t (*SetFrameMetadata)(void*, const OpenNrFrameMetadata*);
  int32_t (*QueryDeviceCapabilities)(void* d3d12_device, OpenNrDeviceCapabilities*);
  int32_t (*GetTimings)(void* session, OpenNrTimings*);
} OpenNrApi;
#ifdef OPEN_NR_RUNTIME_EXPORTS
#define OPEN_NR_EXPORT __declspec(dllexport)
#else
#define OPEN_NR_EXPORT __declspec(dllimport)
#endif
OPEN_NR_EXPORT int32_t OpenNrGetApi(uint32_t version, OpenNrApi* out);
OPEN_NR_EXPORT int32_t OpenNrSetFrameMetadata(void* context, const OpenNrFrameMetadata* metadata);
OPEN_NR_EXPORT int32_t OpenNrQueryDeviceCapabilities(void* d3d12_device, OpenNrDeviceCapabilities* capabilities);
OPEN_NR_EXPORT int32_t OpenNrGetTimings(void* session, OpenNrTimings* timings);
// Failure-only between producer and continuation. Drains prior work and records
// original-color fallback on the actual queue. OK still requires Retire/Drain;
// failure requires keeping the job/session/DLL alive. Recreate after recovery.
OPEN_NR_EXPORT int32_t OpenNrRecoverSubmission(void* session, void* job, void* actual_queue);
#ifdef __cplusplus
}
#endif
