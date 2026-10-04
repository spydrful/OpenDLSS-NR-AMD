# CPU-only RGP container checks

`tools/validate_rgp_container.py` checks bounded container metadata in a frozen `.rgp` file without launching the profiler, a service or the GPU. It uses Python's standard library and supports the public AMD RDF version 3 container schema. It interprets only the PAL `SqttData` version 5, 40-byte header and `TraceError` version 1, 28-byte header. Other chunks remain index metadata only.

This is a structural diagnostic. A passing report does not establish that the trace is complete, healthy or usable by the RGP analysis backend. It provides no dynamic instruction, occupancy, matrix utilization or performance analysis. Keep profiler-capture timings separate from ordinary benchmark timings.

## Run the check

Use Python 3.10 or later from the repository root:

```powershell
python tools/validate_rgp_container.py path/to/capture.rgp --output build/diagnostics/rgp-container.json
```

Freeze the input before checking it: finish writing the capture, close its producer and preserve its identity. To require a known complete-file digest:

```powershell
python tools/validate_rgp_container.py path/to/capture.rgp --output build/diagnostics/rgp-container-pinned.json --expected-sha256 "REPLACE_WITH_64_HEX_DIGITS"
```

The output must be a new file. The script hashes and reads metadata from the same open file and checks file identity, size and modification time afterward. These checks catch common concurrent writes or path replacement. They do not cryptographically exclude concurrent mutation; the frozen-input requirement remains necessary. The SHA256 is an external identity, not an embedded integrity checksum from the RDF format.

Add `--require-unsaturated` for a stricter metadata screening exit code. It still cannot qualify trace completeness:

| Exit code | Meaning |
| --- | --- |
| 0 | Bounded container structure passed. SQTT completeness remains unknown. |
| 1 | Rejected input, unsupported RDF container version, I/O failure or output overwrite attempt. No report is written after input rejection. |
| 2 | With `--require-unsaturated`, at least one SQTT buffer is at capacity, SQTT metadata is missing or unsupported, or a `TraceError` chunk is present. The bounded metadata report is retained. |

## What is checked and emitted

The tool checks the RDF magic/version, reserved fields, 64-byte index entry geometry, UTF-8 identifier encoding, supported compression identifiers and nonnegative ranges bounded by the file size. The index is limited to 16,384 entries. Nonempty referenced ranges must be disjoint in this reader's conservative safety scope; the RDF specification does not explicitly forbid aliases. The index need not be at EOF, and unreferenced bytes are reported.

The JSON contains the capture identity, chunk identifiers and versions, header/data offsets and lengths, compression identifiers, known SQTT header fields and structural warnings. No shader/code-object, tensor, image, event or SQTT token payload is decoded or emitted. `TraceError` reports contain only known header fields, never arbitrary error-message payload text. Hashing streams all input bytes without exposing their contents. Compressed chunk sizes are checked from index metadata; Zstd stream integrity and decompressed contents are not checked.

Unknown SQTT chunk versions or header sizes are visible in `unknownSqttMetadata` and remain uninterpreted. An unknown `TraceError` header is likewise reported without decoding. The script does not treat every other chunk version as supported: these chunks explicitly carry `interpretation: INDEX_METADATA_ONLY`.

## SQTT capacity and truncation limits

The public PAL v5 SQTT header exposes trace-buffer capacity and two flags: instruction-timing enabled and exec-pop tokens enabled. It has **no truncation flag**. The parser checks the recorded logical length against capacity and PAL's 32-byte hardware write units. Equality is reported as `atBufferCapacity` and produces a warning. This identifies a completeness risk; it does not prove truncation. A value below capacity does not establish completeness either. `truncationStatus` is always `UNKNOWN` within this header-only scope.

The v5 40-byte header has 36 named bytes and four native-ABI tail-padding bytes. Padding is ignored, and its contents are never interpreted as flags. `pciIdRaw` is retained as an uninterpreted identifier rather than treated as the GPU's vendor/device ID. These field layouts come from pinned public PAL source; the commercial driver's producer implementation is not independently verified by this tool.

The [historical Q32 capture record](performance/rgp-rx9070xt-20261002.json) already records a genuine RGP viewer warning, `TRUNCATED SQTT`. This validator's `UNKNOWN` status describes its narrower metadata scope and does not replace or invalidate that viewer evidence. It supplies no new performance or completeness claim for that capture. Raw captures remain private and excluded from release payloads.

The [fresh expert metadata screen](performance/rgp-expert-metadata-rx9070xt-20261004.json)
records a 3,700,896-byte capture requested for the isolated original Pair K64
expert expansion. Its 19 container entries and four SQTT headers passed
independent structural review; all recorded SQTT lengths are below capacity.
The profiler logged restoration after its latest peak-clock change and cleanup
left no owned processes. Actual event identity, trace completeness and dynamic
instruction analysis remain unqualified. All 1,440 timing samples from that
profiler run are excluded from performance evidence.

## Schema provenance and CPU tests

The tool records immutable source links and source-file hashes in each report; it downloads or loads no schema files at runtime. It is an independently implemented reader, not AMD's `rdfi` executable or RGP's analysis backend. No AMD library source or profiler binaries are vendored.

- [AMD RDF v3 specification](https://github.com/GPUOpen-Drivers/libamdrdf/blob/1f13ba88fa0753d908d5fba82fe4483406267b80/docs/specification.md) and [little-endian schema](https://github.com/GPUOpen-Drivers/libamdrdf/blob/1f13ba88fa0753d908d5fba82fe4483406267b80/docs/rdf.ksy).
- [PAL SQTT header](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpuPerfExperimentTraceSource.h), [writer](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpuPerfExperimentTraceSource.cpp) and [recorded byte-count/capacity publication](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L2143).
- [PAL error header](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/gpuUtil/palTraceSession.h) and [writer](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/traceSession.cpp).

AMD MIT attribution and permission notices are retained in [RGP_CONTAINER_NOTICE.txt](../tools/RGP_CONTAINER_NOTICE.txt).

Run the CPU-only synthetic tests with:

```powershell
python -m unittest discover -s tests -p test_rgp_container_validation.py
```

The tests cover malformed/truncated headers and indices, out-of-file and overlapping ranges, encoding/compression fields, capacity/error/unknown-version handling, ignored ABI padding, failure exit codes, report preservation and common file-mutation guards. They use synthetic bytes and do not load a model, profiler, service or GPU.
