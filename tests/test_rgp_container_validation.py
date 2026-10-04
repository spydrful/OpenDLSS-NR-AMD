"""CPU-only synthetic bounds/schema tests. Does not touch any GPU capture."""
import ctypes
import importlib.util
import subprocess
import sys
import types
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

TOOL = Path(__file__).resolve().parents[1] / 'tools' / 'validate_rgp_container.py'
spec = importlib.util.spec_from_file_location('validate_rgp_container', TOOL)
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)

def capture(chunks=None, gap=0):
    chunks = [] if chunks is None else chunks
    out = bytearray(v.HEADER.size)
    entries = []
    for name, version, header, data, compression, uncompressed in chunks:
        ho = len(out)
        out.extend(header)
        do = len(out)
        out.extend(data)
        entries.append(v.ENTRY.pack(name.encode().ljust(16, b'\0'), compression, b'\0'*3,
                                    version, ho, len(header), do, len(data), uncompressed))
    out.extend(b'\0' * gap)
    offset = len(out)
    for entry in entries:
        out.extend(entry)
    v.HEADER.pack_into(out, 0, b'AMD_RDF ', 3, 0, offset, len(entries)*64)
    return out

def sqtt(capacity=64, length=32, flags=1, version=5, padding=0):
    header = v.SQTT_FIELDS.pack(196608, 0, 12, 1, 5, 0, capacity, flags) + struct.pack('<I', padding)
    return ('SqttData', version, header, b'\0'*length, 0, 0)

def parse(raw):
    return v.parse(io.BytesIO(raw), len(raw))

class TestValidator(unittest.TestCase):
    def test_empty(self):
        r = parse(capture())
        self.assertEqual(r['rdf']['entryCount'], 0)
        self.assertFalse(r['scope']['traceCompletenessQualified'])

    def test_sqtt_metadata(self):
        r = parse(capture([sqtt()]))
        self.assertTrue(r['sqtt'][0]['instructionTimingEnabled'])
        self.assertFalse(r['sqtt'][0]['execPopTokensEnabled'])
        self.assertFalse(r['sqtt'][0]['atBufferCapacity'])
        self.assertEqual(r['sqtt'][0]['truncationStatus'], 'UNKNOWN')

    def test_capacity_is_warning_not_complete_or_definite_truncation(self):
        r = parse(capture([sqtt(length=64)]))
        self.assertTrue(r['sqtt'][0]['atBufferCapacity'])
        self.assertEqual(len(r['warnings']), 1)
        self.assertFalse(r['scope']['traceCompletenessQualified'])

    def test_tail_padding_ignored(self):
        r = parse(capture([sqtt(padding=205)]))
        self.assertEqual(r['sqtt'][0]['nativeAbiTailPaddingBytesIgnored'], 4)

    def test_flag_combinations(self):
        for flags in range(4):
            r = parse(capture([sqtt(flags=flags)]))['sqtt'][0]
            self.assertEqual(r['instructionTimingEnabled'], bool(flags&1))
            self.assertEqual(r['execPopTokensEnabled'], bool(flags&2))

    def test_unknown_sqtt_versions_do_not_decode(self):
        for version in (0,4,6,0xffffffff):
            r = parse(capture([sqtt(version=version)]))
            self.assertFalse(r['sqtt'])
            self.assertEqual(len(r['unknownSqttMetadata']), 1)

    def test_unknown_sqtt_header_size_does_not_decode(self):
        c=list(sqtt());c[2]=c[2][:36]
        r=parse(capture([tuple(c)]))
        self.assertEqual(r['unknownSqttMetadata'][0]['headerBytes'],36)

    def test_unknown_chunks_and_duplicate_names(self):
        r=parse(capture([('Arbitrary',29,b'xx',b'yy',0,0),('Arbitrary',30,b'',b'zz',0,0)]))
        self.assertEqual(r['chunkCounts']['Arbitrary'],2)
        self.assertEqual(r['chunks'][1]['identifierOrdinal'],1)

    def test_full_16_byte_identifier(self):
        r=parse(capture([('a'*16,1,b'',b'',0,0)]))
        self.assertEqual(r['chunks'][0]['identifier'],'a'*16)

    def test_utf8_identifier(self):
        r=parse(capture([('valid-\u03bb',1,b'',b'',0,0)]))
        self.assertEqual(r['chunks'][0]['identifier'],'valid-\u03bb')

    def test_zstd_is_bounds_only(self):
        c=list(sqtt());c[3]=b'not-a-zstd-stream';c[4]=1;c[5]=32
        r=parse(capture([tuple(c)]))
        self.assertFalse(r['scope']['zstdIntegrityChecked'])
        self.assertEqual(r['sqtt'][0]['logicalTraceBytes'],32)

    def test_index_not_at_eof_is_allowed(self):
        raw=capture([('Empty',1,b'',b'',0,0)])+b'trailing'
        r=parse(raw)
        self.assertFalse(r['rdf']['indexEndsAtEof'])
        self.assertEqual(r['rdf']['unreferencedBytes'],8)

    def test_error_header_without_payload_export(self):
        h=struct.pack('<16sIiI',b'SqttData'.ljust(16,b'\0'),3,-14,1)
        r=parse(capture([('TraceError',1,h,b'private arbitrary error',0,0)]))
        self.assertEqual(r['traceErrors'][0]['palResultCodeRaw'],-14)
        self.assertFalse(r['traceErrors'][0]['payloadDecoded'])
        self.assertNotIn('private arbitrary error',json.dumps(r))

    def test_error_unknown_header_no_decode(self):
        r=parse(capture([('TraceError',2,b'x',b'y',0,0)]))
        self.assertFalse(r['traceErrors'][0]['metadataSchemaSupported'])

    def test_truncated_header(self):
        for size in (0,1,31):
            with self.assertRaises(v.InvalidCapture):parse(bytes(size))

    def test_bad_header_fields(self):
        mutations=[(0,b'BAD_RDF '),(8,struct.pack('<I',4)),(12,struct.pack('<I',1)),
                   (16,struct.pack('<q',-1)),(16,struct.pack('<q',0)),
                   (16,struct.pack('<q',9999)),(24,struct.pack('<q',-64)),
                   (24,struct.pack('<q',63)),(24,struct.pack('<q',v.MAX_INDEX_BYTES+64))]
        for offset,value in mutations:
            raw=capture([('Test',1,b'xx',b'yy',0,0)])
            raw[offset:offset+len(value)]=value
            with self.assertRaises(v.InvalidCapture):parse(raw)

    def test_bad_index_fields(self):
        mutations=[(0,b'a\0b'+b'\0'*13),(0,b'\xff'+b'\0'*15),(16,b'\x02'),
                   (17,b'\x01'),(24,struct.pack('<q',-1)),(32,struct.pack('<q',-1)),
                   (40,struct.pack('<q',-1)),(48,struct.pack('<q',-1)),
                   (56,struct.pack('<q',-1)),(56,struct.pack('<q',1)),
                   (24,struct.pack('<q',9999)),(32,struct.pack('<q',9999)),
                   (40,struct.pack('<q',9999)),(48,struct.pack('<q',9999))]
        for field,value in mutations:
            raw=capture([('Test',1,b'xx',b'yy',0,0)])
            off=v.HEADER.unpack_from(raw)[3]+field
            raw[off:off+len(value)]=value
            with self.assertRaises(v.InvalidCapture):parse(raw)

    def test_overlap_rejected_conservatively(self):
        raw=capture([('Test',1,b'xx',b'yy',0,0)])
        idx=v.HEADER.unpack_from(raw)[3]
        for field,value in [(24,0),(24,idx),(40,32)]:
            variant=bytearray(raw);struct.pack_into('<q',variant,idx+field,value)
            with self.assertRaises(v.InvalidCapture):parse(variant)

    def test_sqtt_invalid_fields(self):
        for item in [sqtt(flags=4),sqtt(capacity=31),sqtt(length=33)]:
            with self.assertRaises(v.InvalidCapture):parse(capture([item]))

    def test_msvc_equivalent_field_offsets_and_size(self):
        class Header(ctypes.LittleEndianStructure):
            _fields_=[('words',ctypes.c_uint32*6),('capacity',ctypes.c_uint64),('flags',ctypes.c_uint32)]
        self.assertEqual(ctypes.sizeof(Header),40)
        self.assertEqual(Header.capacity.offset,24)
        self.assertEqual(Header.flags.offset,32)
        self.assertEqual(v.SQTT_FIELDS.size,36)

    def test_primary_source_provenance_is_static_and_pinned(self):
        self.assertEqual(len(v.PRIMARY_SOURCES),7)
        for source in v.PRIMARY_SOURCES:
            self.assertRegex(source['commit'],r'^[0-9a-f]{40}$')
            self.assertRegex(source['sourceFileSha256'],r'^[0-9a-f]{64}$')
            self.assertIn('/'+source['commit']+'/',source['url'])
            self.assertTrue(source['url'].startswith('https://raw.githubusercontent.com/GPUOpen-Drivers/'))

    def test_expected_hash_rejection_and_valid_file(self):
        with tempfile.TemporaryDirectory(prefix='rdf-cpu-test-') as td:
            p=Path(td)/'test.rgp';p.write_bytes(capture([sqtt()]))
            r=v.validate(p,v.sha256_file(p))
            self.assertEqual(r['capture']['bytes'],p.stat().st_size)
            with self.assertRaises(v.InvalidCapture):v.validate(p,'0'*64)

class FileAndCliTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='rgp-container-cpu-')
        self.root=Path(self.tmp.name)
        self.capture=self.root/'synthetic.rgp'
        self.capture.write_bytes(capture([sqtt(length=64)]))
        self.output=self.root/'report.json'

    def tearDown(self):
        self.tmp.cleanup()

    def cli(self,*extra):
        return subprocess.run([sys.executable,str(TOOL),str(self.capture),'--output',str(self.output),*extra],
                              capture_output=True,text=True,encoding='utf-8')

    def test_cli_valid_metadata_has_no_payload_or_complete_claim(self):
        result=self.cli()
        self.assertEqual(result.returncode,0,result.stderr)
        report=json.loads(self.output.read_text(encoding='utf-8'))
        self.assertFalse(report['scope']['payloadsDecoded'])
        self.assertFalse(report['scope']['dynamicInstructionsOrOccupancyAnalyzed'])
        self.assertFalse(report['scope']['traceCompletenessQualified'])
        self.assertTrue(report['scope']['frozenInputRequired'])
        self.assertFalse(report['schemaProvenance']['runtimeSourceDownloads'])

    def test_cli_capacity_gate_writes_limited_report_and_returns_two(self):
        result=self.cli('--require-unsaturated')
        self.assertEqual(result.returncode,2,result.stderr)
        self.assertEqual(json.loads(self.output.read_text(encoding='utf-8'))['sqtt'][0]['truncationStatus'],'UNKNOWN')

    def test_cli_below_capacity_gate_is_still_unknown(self):
        self.capture.write_bytes(capture([sqtt(length=32)]))
        self.assertEqual(self.cli('--require-unsaturated').returncode,0)
        report=json.loads(self.output.read_text(encoding='utf-8'))
        self.assertFalse(report['sqtt'][0]['atBufferCapacity'])
        self.assertEqual(report['sqtt'][0]['truncationStatus'],'UNKNOWN')
        self.assertFalse(report['scope']['traceCompletenessQualified'])

    def test_cli_input_bytes_unchanged(self):
        before=self.capture.read_bytes()
        self.assertEqual(self.cli().returncode,0)
        self.assertEqual(self.capture.read_bytes(),before)

    def test_cli_refuses_overwrite_preserving_bytes(self):
        self.output.write_bytes(b'existing report')
        result=self.cli()
        self.assertEqual(result.returncode,1)
        self.assertEqual(self.output.read_bytes(),b'existing report')

    def test_cli_capture_output_alias_refuses_without_mutation(self):
        original=self.capture.read_bytes()
        self.output=self.capture
        self.assertEqual(self.cli().returncode,1)
        self.assertEqual(self.capture.read_bytes(),original)

    def test_cli_malformed_hash_does_not_write(self):
        for bad in ('x'*64,'0'*63,'0'*65):
            with self.subTest(hash=bad):
                self.assertEqual(self.cli('--expected-sha256',bad).returncode,1)
                self.assertFalse(self.output.exists())

    def test_cli_wrong_hash_does_not_write(self):
        self.assertEqual(self.cli('--expected-sha256','0'*64).returncode,1)
        self.assertFalse(self.output.exists())

    def test_cli_correct_hash(self):
        self.assertEqual(self.cli('--expected-sha256',v.sha256_file(self.capture).upper()).returncode,0)

    def test_cli_truncated_file_does_not_write(self):
        self.capture.write_bytes(b'AMD_RDF ')
        self.assertEqual(self.cli().returncode,1)
        self.assertFalse(self.output.exists())

    def test_cli_unknown_metadata_visibly_gates(self):
        self.capture.write_bytes(capture([sqtt(version=6)]))
        self.assertEqual(self.cli('--require-unsaturated').returncode,2)
        report=json.loads(self.output.read_text(encoding='utf-8'))
        self.assertEqual(report['unknownSqttMetadata'][0]['version'],6)
        self.assertTrue(report['warnings'])

    def test_cli_missing_sqtt_visibly_gates(self):
        self.capture.write_bytes(capture())
        self.assertEqual(self.cli('--require-unsaturated').returncode,2)
        report=json.loads(self.output.read_text(encoding='utf-8'))
        self.assertTrue(any('NO_SQTT_CHUNKS' in warning for warning in report['warnings']))

    def test_cli_trace_error_visibly_gates(self):
        h=struct.pack('<16sIiI',b'SqttData'.ljust(16,b'\0'),3,-14,1)
        self.capture.write_bytes(capture([sqtt(length=32),('TraceError',1,h,b'secret event string',0,0)]))
        self.assertEqual(self.cli('--require-unsaturated').returncode,2)
        report=self.output.read_text(encoding='utf-8')
        self.assertNotIn('secret event string',report)

    def test_single_handle_detects_write_after_hash(self):
        original=v.sha256_stream
        def mutate(stream):
            result=original(stream)
            with self.capture.open('ab') as f:f.write(b'x')
            return result
        with mock.patch.object(v,'sha256_stream',side_effect=mutate):
            with self.assertRaisesRegex(v.InvalidCapture,'changed or was replaced'):
                v.validate(self.capture)

    def test_single_handle_detects_path_replacement_identity(self):
        actual=self.capture.stat()
        changed=types.SimpleNamespace(st_dev=actual.st_dev,st_ino=actual.st_ino+1,
                                      st_size=actual.st_size,st_mtime_ns=actual.st_mtime_ns)
        with mock.patch.object(Path,'stat',return_value=changed):
            with self.assertRaisesRegex(v.InvalidCapture,'changed or was replaced'):
                v.validate(self.capture)

if __name__=='__main__':
    unittest.main(verbosity=2)
