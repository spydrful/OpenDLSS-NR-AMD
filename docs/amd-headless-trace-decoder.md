# Headless RDNA4 trace-decoder investigation

AMD's public `rocprof-trace-decoder` compiled as an x64 Windows static library,
and a consumer of its public handle API passed all 22 lifecycle checks. This is
CPU build and API feasibility evidence. Our RGP captures have not been decoded
with it; PAL compatibility, captured event identity and dynamic instruction
analysis remain unqualified.

The [CPU feasibility record](performance/decoder-windows-feasibility-20261004.json)
contains the tested identities and bounded results.

The purpose is to investigate why the preserving FP8 kernels remain expensive.
The qualified target network result remains **47.74616 ms median**. These CPU
checks provide no inference, bridge or game performance improvement.

## Frozen source and build

The MIT-licensed source is
[AMD's rocprof-trace-decoder at commit 4bab228](https://github.com/ROCm/rocm-systems/tree/4bab22839f1badc03e1c254421665d4ae551c18b/projects/rocprof-trace-decoder).
Its compiled version is **0.2.2**. The local source inventory verified 125 text
files against Git blob identities and SHA256 hashes. Upstream binary trace
fixtures were not downloaded for this build.

| Component | Tested value |
| --- | --- |
| CMake | 4.4.2 |
| Compiler | Visual Studio 2022 Build Tools, toolset 14.44.35207, compiler 19.44.35228.0, x64 |
| Windows SDK | 10.0.26100.0 |
| Target | `rocprof-trace-decoder-static`, Release |
| Optional components | Tests, Python, LLVM disassembly, COMGR, architecture model and Doxygen disabled |
| Explicit C++ flags | `/utf-8 /EHsc` |
| Generated exception handling | `ExceptionHandling=Sync` |

The initial compilation omitted `/EHsc` because our explicit C++ flags replaced
CMake's defaults. Its compiler warnings were retained, and that library was
excluded from API execution. A fresh build restored `/EHsc` and verified the
generated compiler setting before compilation. It retained one `C4244` warning;
the exception-unwinding warnings were gone.

With a checkout of the pinned source and the tested toolchain installed, the
core configuration can be reproduced with:

```powershell
cmake -S path/to/rocprof-trace-decoder -B build/decoder-static `
  -G 'Visual Studio 17 2022' -A 'x64,version=10.0.26100.0' `
  -T 'v143,host=x64,version=14.44.35207' `
  -DBUILD_TESTS=OFF -DBUILD_UNIT_TESTS=OFF -DBUILD_PYTHON=OFF `
  -DDISABLE_COMGR=ON -DUSE_LLVM_DISASM=OFF -DARCH_MODEL=OFF `
  -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON `
  '-DCMAKE_CXX_FLAGS=/utf-8 /EHsc' `
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
cmake --build build/decoder-static --config Release `
  --target rocprof-trace-decoder-static --parallel 1 -- /m:1 /nr:false
```

Our runs additionally fixed the generator instance/MSBuild path, cleared
toolchain/project hook settings, disabled package registries and directory build
imports, bounded configure/compile time, and verified process cleanup. The
commands above describe the core build, rather than reproduce that supervision.
They do not install or run a decoder. Byte identity across other source paths or
SDK installations has not been established.

## Public API and synthetic parsing checks

The linked consumer verified version reporting, null arguments, two distinct
handles, invalid and retired handles, missing callbacks, unavailable disassembly,
empty parsing with a custom callback, callback clearing and destruction. All
22 checks passed with zero trace/ISA callbacks and empty runtime stderr. The
consumer imported the Windows CRT and Kernel32, with no LLVM, COMGR or separate
decoder DLL dependency.

The no-disassembly build requires a custom ISA callback for full parsing. The
MSVC quick-scan route is unavailable in this source; the public handle parse API
is the route under investigation. An empty-input lifecycle test establishes no
token, instruction or code-object correctness.

A separate consumer then passed all **16 assertions** for two upstream-derived
synthetic GFX12 streams and two error cases through the public handle parse API:

| Synthetic case | Logical bytes supplied | Result checked |
| --- | --- | --- |
| Realtime timestamp | 16 | GFXIP then one exact REALTIME record |
| Complete wave lifecycle | 22 | GFXIP then two exact OCCUPANCY records, times 1 and 8 |
| Unsupported header version 6 | 8 | Invalid shader data, no callbacks |
| Short header | 7 | Invalid shader data, no callbacks |

The lifecycle stream's wave belongs to a different WGP/SIMD from its synthetic
header's trace target. It therefore exercises token timing and occupancy record
forwarding without instruction stitching. All cases made zero ISA requests;
runtime stderr was empty. This covers known synthetic formats, rather than PC
mapping, reconstructed instructions or measured GPU occupancy.

## Remaining work

Real-capture work must prove the existing raw hardware header format,
PAL/RDF extraction boundaries, code-object/PC mapping and marker behavior.
Headers must never be fabricated or substituted to make a capture parse.
Decoder success alone cannot establish trace completeness: lost-data,
incomplete-wave, incomplete-stitch and unknown-PC results must remain visible.

Raw captures, NVIDIA DLLs, imported weights and game captures remain outside
commits and release packages. The AMD runtime, lifecycle ABI, tuning cache and
release defaults are unchanged. NR stays disabled by default while the release
gates remain unmet.
