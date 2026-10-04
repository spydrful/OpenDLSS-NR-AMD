"""Bounded RDF-v3/PAL SQTT-v5 metadata validator. Never decodes chunk payloads."""
import argparse
from collections import Counter
import hashlib
import json
import os
import re
import stat
from pathlib import Path
import struct
import sys

HEADER = struct.Struct('<8sIIqq')
ENTRY = struct.Struct('<16sB3sIqqqqq')
SQTT_FIELDS = struct.Struct('<6IQI')  # 36 named bytes; Windows ABI adds 4 tail-padding bytes.
MAX_INDEX_BYTES = 64 * 16384

class InvalidCapture(ValueError):
    pass

def require(value, message):
    if not value:
        raise InvalidCapture(message)

def sha256_stream(stream):
    stream.seek(0)
    h = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        h.update(block)
    return h.hexdigest()

def sha256_file(path):
    with path.open('rb') as stream:
        return sha256_stream(stream)

# Independently implemented reader; no AMD source is vendored or loaded at runtime.
# Schema identities are provenance only, not installed-driver/backend identity.
# AMD MIT attribution is retained in tools/RGP_CONTAINER_NOTICE.txt.
PRIMARY_SOURCES = [{'repository': 'GPUOpen-Drivers/libamdrdf',
  'commit': '1f13ba88fa0753d908d5fba82fe4483406267b80',
  'url': 'https://raw.githubusercontent.com/GPUOpen-Drivers/libamdrdf/1f13ba88fa0753d908d5fba82fe4483406267b80/docs/specification.md',
  'sourceFileBytes': 2471,
  'sourceFileSha256': 'd43a56d32c35d5bf92cfb9f1ddca636b3b8e4876bb04101467e9bc869077a8c7'},
 {'repository': 'GPUOpen-Drivers/libamdrdf',
  'commit': '1f13ba88fa0753d908d5fba82fe4483406267b80',
  'url': 'https://raw.githubusercontent.com/GPUOpen-Drivers/libamdrdf/1f13ba88fa0753d908d5fba82fe4483406267b80/docs/rdf.ksy',
  'sourceFileBytes': 1424,
  'sourceFileSha256': 'f02122c531d1374225405bf6c5d24d8ebb4fae5ec952869ed2ff41574a420be7'},
 {'repository': 'GPUOpen-Drivers/pal',
  'commit': 'c5e800072a32f68b6ccc4422936d96167c6e0728',
  'url': 'https://raw.githubusercontent.com/GPUOpen-Drivers/pal/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpuPerfExperimentTraceSource.h',
  'sourceFileBytes': 6543,
  'sourceFileSha256': 'ed219731c2c45ab994e15bdf2656ff3d6446677d17fe6a992f2925dafdf64466'},
 {'repository': 'GPUOpen-Drivers/pal',
  'commit': 'c5e800072a32f68b6ccc4422936d96167c6e0728',
  'url': 'https://raw.githubusercontent.com/GPUOpen-Drivers/pal/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpuPerfExperimentTraceSource.cpp',
  'sourceFileBytes': 27415,
  'sourceFileSha256': '74d648fbc7a09d4090688c435aa36449c837dff3e3be3cb03dc3380804b3244d'},
 {'repository': 'GPUOpen-Drivers/pal',
  'commit': 'c5e800072a32f68b6ccc4422936d96167c6e0728',
  'url': 'https://raw.githubusercontent.com/GPUOpen-Drivers/pal/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp',
  'sourceFileBytes': 214614,
  'sourceFileSha256': 'b1724567a4c8df797805a24b8954e194d86958f992b0a5ac1005575ae3fd65e2'},
 {'repository': 'GPUOpen-Drivers/pal',
  'commit': 'c5e800072a32f68b6ccc4422936d96167c6e0728',
  'url': 'https://raw.githubusercontent.com/GPUOpen-Drivers/pal/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/gpuUtil/palTraceSession.h',
  'sourceFileBytes': 35330,
  'sourceFileSha256': '9c45ce51d8ead06167cf5e6168a3e440a50b62b3ac2e78100f47ce7598554f46'},
 {'repository': 'GPUOpen-Drivers/pal',
  'commit': 'c5e800072a32f68b6ccc4422936d96167c6e0728',
  'url': 'https://raw.githubusercontent.com/GPUOpen-Drivers/pal/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/traceSession.cpp',
  'sourceFileBytes': 30759,
  'sourceFileSha256': '007c9d5302c185101102258a46e92c414c1e0719f485ea1dea330954ba27946a'}]

def identifier(raw):
    head, sep, tail = raw.partition(b'\0')
    require(not sep or not tail.strip(b'\0'), 'Nonzero byte after identifier terminator')
    try:
        return head.decode('utf-8', errors='strict')
    except UnicodeDecodeError as error:
        raise InvalidCapture('Identifier is not valid UTF-8') from error

def bounded_range(offset, size, file_size, label):
    require(offset >= 0 and size >= 0, f'Negative {label} offset/size')
    require(offset <= file_size and size <= file_size - offset, f'{label} range exceeds EOF')
    return (offset, offset + size, label)

def read_exact(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    require(len(data) == size, 'File shortened during metadata read')
    return data

def parse(stream, file_size):
    require(file_size >= HEADER.size, 'Truncated RDF header')
    magic, version, reserved, offset, size = HEADER.unpack(read_exact(stream, 0, HEADER.size))
    require(magic == b'AMD_RDF ', 'Unsupported RDF identifier')
    require(version == 3, 'Unsupported RDF version; only documented version 3 is supported')
    require(reserved == 0, 'Nonzero RDF header reserved field')
    require(size % ENTRY.size == 0, 'Index size is not a multiple of 64')
    require(size <= MAX_INDEX_BYTES, 'Index exceeds validator 16384-entry safety limit')
    index_range = bounded_range(offset, size, file_size, 'index')
    require(offset >= HEADER.size, 'Index overlaps RDF header')
    index = read_exact(stream, offset, size)
    entries, intervals, ordinals = [], [(0, HEADER.size, 'RDF header'), index_range], Counter()
    for i in range(size // ENTRY.size):
        raw, compression, reserved3, chunk_version, ho, hs, do, ds, us = ENTRY.unpack_from(index, i * ENTRY.size)
        name = identifier(raw)
        require(reserved3 == b'\0' * 3, f'Chunk {i} reserved index bytes are nonzero')
        require(compression in (0, 1), f'Chunk {i} uses unsupported compression')
        require(us >= 0, f'Chunk {i} negative decompressed size')
        require(compression != 0 or us == 0, f'Chunk {i} uncompressed size field must be zero')
        intervals.extend([bounded_range(ho, hs, file_size, f'chunk {i} header'),
                          bounded_range(do, ds, file_size, f'chunk {i} data')])
        entry = {'index': i, 'identifier': name, 'identifierOrdinal': ordinals[name],
                 'version': chunk_version, 'compression': 'none' if compression == 0 else 'zstd',
                 'headerOffset': ho, 'headerBytes': hs, 'dataOffset': do, 'storedDataBytes': ds,
                 'uncompressedDataBytes': us, 'payloadDecoded': False, 'interpretation': 'INDEX_METADATA_ONLY'}
        ordinals[name] += 1
        entries.append(entry)
    # Conservative validator scope: reject aliases/overlap; RDF specification does not explicitly forbid aliasing.
    nonempty = sorted((start, end, label) for start, end, label in intervals if end > start)
    for left, right in zip(nonempty, nonempty[1:]):
        require(left[1] <= right[0], f'Overlapping ranges unsupported by bounded validator: {left[2]}, {right[2]}')
    sqtt, unknown_sqtt, trace_errors, metadata_bytes = [], [], [], HEADER.size + size
    for entry in entries:
        if entry['identifier'] == 'SqttData':
            if entry['version'] != 5 or entry['headerBytes'] != 40:
                unknown_sqtt.append({'index': entry['index'], 'version': entry['version'],
                                     'headerBytes': entry['headerBytes'], 'status': 'UNSUPPORTED_METADATA_SCHEMA'})
                continue
            raw = read_exact(stream, entry['headerOffset'], 40)
            metadata_bytes += 40
            pci, engine, sqtt_ver, spec, api, wgp, capacity, flags = SQTT_FIELDS.unpack_from(raw)
            require(flags & ~3 == 0, 'SQTT v5 reserved flag bits are nonzero')
            logical_bytes = entry['storedDataBytes'] if entry['compression'] == 'none' else entry['uncompressedDataBytes']
            require(logical_bytes <= capacity, 'SQTT logical data size exceeds recorded trace-buffer capacity')
            require(logical_bytes % 32 == 0, 'SQTT size not in PAL hardware 32-byte write units')
            sqtt.append({'index': entry['index'], 'version': 5, 'pciIdRaw': pci,
                         'shaderEngine': engine, 'sqttVersionRaw': sqtt_ver,
                         'instrumentationVersionSpec': spec, 'instrumentationVersionApi': api,
                         'wgpIndex': wgp, 'traceBufferCapacityBytes': capacity,
                         'logicalTraceBytes': logical_bytes, 'atBufferCapacity': logical_bytes == capacity,
                         'instructionTimingEnabled': bool(flags & 1), 'execPopTokensEnabled': bool(flags & 2),
                         'truncationFlagAvailableInSchema': False, 'truncationStatus': 'UNKNOWN',
                         'nativeAbiTailPaddingBytesIgnored': 4,
                         'payloadLengthChecked': True, 'payloadSemanticsChecked': False})
        elif entry['identifier'] == 'TraceError':
            # Headers only; deliberately do not expose arbitrary diagnostic strings from its payload.
            detail = {'index': entry['index'], 'version': entry['version'], 'payloadDecoded': False}
            if entry['version'] == 1 and entry['headerBytes'] == 28:
                raw = read_exact(stream, entry['headerOffset'], 28)
                metadata_bytes += 28
                failed_id, chunk_index, result_code, payload_type = struct.unpack('<16sIiI', raw)
                detail.update({'failingChunk': identifier(failed_id), 'chunkIndex': chunk_index,
                               'palResultCodeRaw': result_code, 'payloadTypeRaw': payload_type})
            else:
                detail['metadataSchemaSupported'] = False
            trace_errors.append(detail)
    warnings = []
    if not ordinals['SqttData']:
        warnings.append('NO_SQTT_CHUNKS: no SQTT header metadata is available')
    if any(x['atBufferCapacity'] for x in sqtt):
        warnings.append('SQTT_BUFFER_AT_CAPACITY: completeness/truncation cannot be established from metadata')
    if unknown_sqtt:
        warnings.append('UNSUPPORTED_SQTT_METADATA: no SQTT completeness assertion')
    if trace_errors:
        warnings.append('TRACE_ERROR_CHUNKS_PRESENT: container validity does not imply healthy trace')
    return {'format': 'OpenNR-RGP-bounded-container-metadata-v1',
            'status': 'PASS_BOUNDED_CONTAINER_STRUCTURE',
            'rdf': {'version': version, 'indexOffset': offset, 'indexBytes': size,
                    'entryCount': len(entries), 'indexEndsAtEof': offset + size == file_size,
                    'allReferencedRangesInFile': True, 'allNonemptyRangesDisjoint': True,
                    'referencedBytes': sum(end - start for start, end, _ in nonempty),
                    'unreferencedBytes': file_size - sum(end - start for start, end, _ in nonempty)},
            'chunkCounts': dict(ordinals), 'chunks': entries, 'sqtt': sqtt,
            'unknownSqttMetadata': unknown_sqtt, 'traceErrors': trace_errors, 'warnings': warnings,
            'metadataReadBytes': metadata_bytes,
            'scope': {'payloadsDecoded': False, 'zstdIntegrityChecked': False,
                      'shaderCodeExported': False, 'tensorsOrImagesExported': False,
                      'sqttTokensDecoded': False, 'traceCompletenessQualified': False,
                      'dynamicInstructionsOrOccupancyAnalyzed': False,
                      'checksumInRdfSchema': False,
                      'sqttFlagLayout': 'PAL Windows little-endian u32 bitfields: bits0/1; no truncation bit',
                      'unknownVersionsFailMetadataInterpretation': True,
                      'overlapRejectionIsConservativeValidatorRestriction': True}}

def file_identity(value):
    return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns

def validate(path, expected_sha256=None):
    path = Path(path)
    if expected_sha256 is not None:
        require(re.fullmatch(r'[0-9a-fA-F]{64}', expected_sha256) is not None,
                'Expected SHA256 must be exactly 64 hexadecimal characters')
    # Keep the same open file for identity hashing and metadata reads. This detects
    # common replacement/write races but intentionally assumes a frozen capture.
    with path.open('rb') as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode), 'Capture must be a regular file')
        identity = sha256_stream(stream)
        if expected_sha256 is not None:
            require(identity == expected_sha256.lower(), 'Capture SHA256 differs from expected immutable identity')
        report = parse(stream, before.st_size)
        after = os.fstat(stream.fileno())
        require(file_identity(before) == file_identity(after) == file_identity(path.stat()),
                'Capture changed or was replaced during validation')
    report['capture'] = {'path': str(path.resolve()), 'bytes': before.st_size, 'sha256': identity,
                         'sha256IsExternalIdentityNotEmbeddedIntegrityProof': True}
    report['schemaProvenance'] = {'sources': PRIMARY_SOURCES, 'runtimeSourceDownloads': False,
                                 'commercialDriverProducerSourceVerified': False}
    report['scope']['frozenInputRequired'] = True
    report['scope']['concurrentMutationCryptographicallyExcluded'] = False
    report['validator'] = {'path': str(Path(__file__).resolve()), 'sha256': sha256_file(Path(__file__))}
    return report

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--expected-sha256')
    ap.add_argument('--require-unsaturated', action='store_true',
                    help='Exit 2 for at-capacity or unknown SQTT metadata; still cannot establish completeness.')
    args = ap.parse_args()
    require(not args.output.exists(), 'Refusing to overwrite report')
    report = validate(args.capture, args.expected_sha256)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        output.write(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'entries': report['rdf']['entryCount'],
                      'sqttChunks': len(report['sqtt']), 'warnings': report['warnings'],
                      'report': str(args.output.resolve()), 'reportSha256': sha256_file(args.output)}))
    if args.require_unsaturated and (not report['sqtt'] or report['unknownSqttMetadata'] or
                                    report['traceErrors'] or any(x['atBufferCapacity'] for x in report['sqtt'])):
        return 2
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (InvalidCapture, OSError, ValueError, KeyError) as error:
        print(f'VALIDATOR_REJECTED: {error}', file=sys.stderr)
        sys.exit(1)
