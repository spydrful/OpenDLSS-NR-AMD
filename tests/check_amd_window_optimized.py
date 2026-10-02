"""CPU-only checks for the compact window kernel; GPU equality remains required.

Usage: python tests/check_amd_window_optimized.py <compiled SPIR-V>
"""

import math
import random
import struct
import sys
from collections import Counter
from pathlib import Path


def round_half(value):
    try:
        return struct.unpack("<e", struct.pack("<e", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def reference_sum(row):
    # Independently follow the existing scalar helper's pair/parity loops.
    parity_sums = []
    for parity in range(2):
        branches = []
        for pair in range(4):
            key = pair * 2 + parity
            terms = [round_half(row[key + offset] + row[key + offset + 8])
                     for offset in (0, 16, 32, 48)]
            value = terms[0]
            for term in terms[1:]:
                value = round_half(value + term)
            branches.append(value)
        value = branches[0]
        for term in branches[1:]:
            value = round_half(value + term)
        parity_sums.append(value)
    return round_half(parity_sums[0] + parity_sums[1])


def lane_sum(row):
    pieces = []
    for lane in range(8):
        a = round_half(row[lane] + row[lane + 8])
        b = round_half(row[lane + 16] + row[lane + 24])
        c = round_half(row[lane + 32] + row[lane + 40])
        d = round_half(row[lane + 48] + row[lane + 56])
        pieces.append(round_half(round_half(round_half(a + b) + c) + d))
    parity = [round_half(round_half(round_half(pieces[lane] + pieces[lane ^ 2])
                                   + pieces[lane ^ 4]) + pieces[lane ^ 6])
              for lane in range(8)]
    # Every lane gathers team lanes 0 and 1, as the two XOR broadcasts do.
    return [round_half(parity[lane ^ lane] + parity[lane ^ (lane ^ 1)])
            for lane in range(8)]


def check_reduction():
    rng = random.Random(9070)
    rows = [[0.0] * 64, [2.0**-14] * 64, [9.75] * 64]
    rows += [[2.0**-14 if key & 1 else 9.75 for key in range(64)]]
    # nrWindowExp emits positive finite half values from 0x0400 through 0x48e0.
    for _ in range(4096):
        rows.append([struct.unpack("<e", struct.pack("<H", rng.randrange(0x400, 0x48E1)))[0]
                     for _ in range(64)])
    for row in rows:
        expected = struct.pack("<f", reference_sum(row))
        assert all(struct.pack("<f", value) == expected for value in lane_sum(row))
    print(f"PASS fixed half reduction: {len(rows)} rows, eight lane results each")


def check_layout():
    scores = Counter()
    output = Counter()
    scratch = [set() for _ in range(4)]
    for wave in range(4):
        for tile in range(wave, 16, 4):
            query_tile, key_tile = divmod(tile, 4)
            for lane in range(32):
                for index in range(lane, 256, 32):
                    row, column = divmod(index, 16)
                    scores[(query_tile * 16 + row) * 64 + key_tile * 16 + column] += 1
                    scratch[wave].add(wave * 256 + index)
        for tile in range(wave, 8, 4):
            query_tile, channel_tile = divmod(tile, 2)
            for index in range(256):
                row, column = divmod(index, 16)
                output[(query_tile * 16 + row) * 32 + channel_tile * 16 + column] += 1
    assert scores == Counter({index: 1 for index in range(4096)})
    assert output == Counter({index: 1 for index in range(2048)})
    assert all(len(region) == 256 for region in scratch)
    assert len(set().union(*scratch)) == 1024
    weights = Counter()
    for wave in range(4):
        for lane in range(32):
            for batch in range(4):
                row = wave * 4 + lane // 8 + batch * 16
                for key in range(lane & 7, 64, 8):
                    weights[row * 64 + key] += 1
    assert weights == Counter({index: 1 for index in range(4096)})
    print("PASS tile/weight ownership: every score, weight and output once; disjoint wave scratch")


def check_spirv(path):
    binary = Path(path).read_bytes()
    words = struct.unpack(f"<{len(binary) // 4}I", binary)
    assert words[0] == 0x07230203
    types, constants, names, variables, spec_ids, capabilities = {}, {}, {}, [], {}, []
    modes = []
    position = 5
    while position < len(words):
        count, opcode = words[position] >> 16, words[position] & 0xFFFF
        assert count and position + count <= len(words)
        args = words[position + 1:position + count]
        if opcode == 5:  # OpName
            names[args[0]] = struct.pack(f"<{len(args) - 1}I", *args[1:]).split(b"\0", 1)[0].decode()
        elif opcode in (21, 22, 28, 32):  # Integer, float, array, pointer types
            types[args[0]] = (opcode, args[1:])
        elif opcode in (43, 50):  # OpConstant / OpSpecConstant scalar defaults
            constants[args[1]] = args[2]
        elif opcode == 59 and args[2] == 4:  # OpVariable Workgroup
            variables.append((args[1], args[0]))
        elif opcode == 71 and args[1] == 1:  # SpecId
            spec_ids[args[0]] = args[2]
        elif opcode in (16, 331):
            modes.append((opcode, args))
        elif opcode == 17:
            capabilities.append(args[0])
        position += count

    def byte_size(type_id):
        opcode, args = types[type_id]
        if opcode in (21, 22):
            return args[0] // 8
        if opcode == 28:
            return byte_size(args[0]) * constants[args[1]]
        if opcode == 32:
            return byte_size(args[1])
        raise AssertionError(f"unsupported shared type {opcode}")

    shared = {names[var]: byte_size(type_id) for var, type_id in variables}
    expected = {"nrAwQs": 2048, "nrAwKs": 2048, "nrAwVs": 2048,
                "nrAwWeights": 4096, "nrAwScores": 8192, "nrAwScratch": 4096}
    assert shared == expected, shared
    assert sum(shared.values()) == 22528
    assert any(spec == 10 and constants[identifier] == 16 for identifier, spec in spec_ids.items())
    assert 65 in capabilities  # GroupNonUniformShuffle, required by ShuffleXor.
    local_sizes = []
    for opcode, args in modes:
        if opcode == 16 and args[1] == 17:
            local_sizes.append(tuple(args[2:5]))
        elif opcode == 331 and args[1] == 38:
            local_sizes.append(tuple(constants[value] for value in args[2:5]))
    assert (128, 1, 1) in local_sizes, local_sizes
    print(f"PASS SPIR-V declarations: {sum(shared.values())} bytes (22 KiB), local128, spec10 default16")
    print("Declared allocation only; driver-compiled resources and GPU equality are separate gates.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    check_reduction()
    check_layout()
    check_spirv(sys.argv[1])
