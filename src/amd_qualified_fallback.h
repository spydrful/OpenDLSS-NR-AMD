#pragma once
// Frozen GPU-to-GPU evidence at 320x320 and 1707x960 qualifies the K16,
// N16/K16, 64-query compact route for this exact model/driver/shader set.
// SHA-256 identities are metadata; no model weights are embedded here.
namespace amd::qualified {
inline constexpr const char* device = "1002:7550";
inline constexpr const char* driver = "AMD proprietary driver|26.9.1 (LLPC)|8389003";
inline constexpr const char* model = "163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e";
inline constexpr const char* shaders = "e71855a4554be2cfc62a36faa4de7522b052a04a5148a54c46ab45989b188621";
inline constexpr const char* baseline = "360c9488cd87c7e1de22d6b56f051921e9d4408f2efbd70ad1d3b01084ad3dae";
}
