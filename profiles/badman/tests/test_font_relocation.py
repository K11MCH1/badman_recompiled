"""Synthetic relocation-field tests; no commercial ELF, snapshots or Ghidra required."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import unittest

PROFILE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('badman_reconstruct_font', PROFILE / 'tools/reconstruct_font.py')
font = importlib.util.module_from_spec(spec)
spec.loader.exec_module(font)


def operation(offset, mask, addend, segment=0):
    return {'segment': segment, 'offset': offset, 'mask': mask, 'addend': addend}


def synthetic_elf(first=None, second=None):
    first = first if first is not None else bytes(32)
    second = second if second is not None else bytes(32)
    second_offset = (0x100 + len(first) + 15) & ~15
    result = bytearray(second_offset + len(second))
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<16sHHIIIIIHHHHHH', result, 0, ident, 0xffa0, 8, 1, 0xffffffff, 52, 0, 0, 52, 32, 2, 0, 0, 0)
    struct.pack_into('<8I', result, 52, 1, 0x100, 0, 0, len(first), len(first) + 16, 5, 16)
    struct.pack_into('<8I', result, 84, 1, second_offset, 0x8020, 0, len(second), len(second) + 16, 6, 16)
    result[0x100:0x100 + len(first)] = first
    result[second_offset:second_offset + len(second)] = second
    return bytes(result)


class RelocationTests(unittest.TestCase):
    def setUp(self):
        self.base = 0x09fd0000
        self.layout = [{'vaddr': 0, 'filesz': 32, 'memsz': 32}, {'vaddr': 0x8020, 'filesz': 32, 'memsz': 32}]
        self.words = [0xffff0010, (2 << 26) | 0x03fffff0, (3 << 26) | 0x12,
                      (15 << 26) | (2 << 16) | 0xffff,
                      (9 << 26) | (5 << 21) | (4 << 16) | 0xffff,
                      (35 << 26) | (6 << 21) | (3 << 16) | 7,
                      (36 << 26) | (6 << 21) | (3 << 16) | 0x10,
                      (49 << 26) | (6 << 21) | (3 << 16) | 0x10]
        self.parts = [struct.pack('<8I', *self.words), struct.pack('<8I', 0x1000, *([0] * 7))]

    def apply(self, rows):
        return font.apply_operations(self.parts, self.layout, rows, self.base)

    def test_pointer_jump_high_low_and_modular_carry(self):
        rows = [operation(0, 0xffffffff, self.base), operation(4, 0x3ffffff, self.base >> 2),
                operation(8, 0x3ffffff, self.base >> 2), operation(12, 0xffff, self.base >> 16),
                operation(16, 0xffff, 0x8020), operation(20, 0xffff, 0),
                operation(24, 0xffff, 0x8020), operation(28, 0xffff, 0),
                operation(0, 0xffffffff, self.base + 0x8020, segment=1)]
        result = self.apply(rows)
        words = struct.unpack('<8I', result[0])
        self.assertEqual(words[0], 0x09fc0010)
        self.assertEqual(words[1] >> 26, 2)
        self.assertEqual(words[1] & 0x3ffffff, 0x027f3ff0)
        self.assertEqual(words[2] >> 26, 3)
        self.assertEqual(words[2] & 0x3ffffff, 0x027f4012)
        self.assertEqual(words[3] & 0xffff, 0x09fc)
        self.assertEqual(words[4] & 0xffff, 0x801f)
        self.assertEqual(words[5], self.words[5])
        self.assertEqual(words[6] & 0xffff, 0x8030)
        self.assertEqual(words[7], self.words[7])
        for index in range(3, 8):
            self.assertEqual(words[index] & 0xffff0000, self.words[index] & 0xffff0000)
        self.assertEqual(struct.unpack_from('<I', result[1])[0], 0x09fd9020)
        self.assertEqual(self.parts[0], struct.pack('<8I', *self.words))

    def test_bounds_alignment_duplicates_and_failure_atomicity(self):
        for row in [operation(2, 0xffffffff, self.base), operation(32, 0xffffffff, self.base),
                    operation(0, 0xffffffff, self.base, segment=2), operation(-4, 0xffffffff, self.base)]:
            with self.subTest(row=row), self.assertRaises(ValueError):
                self.apply([row])
        original = list(self.parts)
        with self.assertRaises(ValueError):
            self.apply([operation(0, 0xffffffff, self.base), operation(0, 0xffffffff, self.base + 0x8020)])
        self.assertEqual(self.parts, original)

    def test_reject_arbitrary_byte_masks_words_and_nonrelocation_addends(self):
        for row in [operation(0, 0xff, 1), operation(0, 0xffffffff, 1),
                    operation(4, 0x3ffffff, 1), operation(16, 0xffff, 7),
                    {**operation(0, 0xffffffff, self.base), 'destination_word': 0},
                    {'segment': 0, 'offset': 0, 'mask': 0xffffffff}]:
            with self.subTest(row=row), self.assertRaises(ValueError):
                self.apply([row])
        with self.assertRaises(ValueError):
            self.apply([])
        with self.assertRaises(ValueError):
            self.apply([operation(0, 0xffffffff, True)])

    def test_instruction_field_masks_require_documented_opcode_families(self):
        with self.assertRaises(ValueError):
            self.apply([operation(16, 0x3ffffff, self.base >> 2)])
        parts = [bytes(32), bytes(32)]
        with self.assertRaises(ValueError):
            font.apply_operations(parts, self.layout, [operation(0, 0xffff, 0)], self.base)

    def test_parse_load_layout_and_zero_bss_extent(self):
        rows = font.segments(synthetic_elf())
        self.assertEqual([(r['vaddr'], r['filesz'], r['memsz']) for r in rows], [(0, 32, 48), (0x8020, 32, 48)])
        self.assertEqual([r['flags'] for r in rows], [5, 6])

    def test_reject_malformed_elf_and_overlapping_or_unbounded_segments(self):
        for offset, value in [(28, 0xffffff00), (52 + 16, 100000), (52 + 20, 0xffffffff),
                              (84 + 8, 16), (84 + 4, 0x110)]:
            data = bytearray(synthetic_elf())
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                font.segments(data)
        for data in [b'', b'not an ELF', b'\x7fELF' + bytes(100)]:
            with self.assertRaises(ValueError):
                font.segments(data)

    def test_checked_in_recipe_contains_only_relocation_facts(self):
        recipe = json.loads((PROFILE / 'config/font_relocations.json').read_text())
        identity = json.loads((PROFILE / 'config/font_module.json').read_text())
        self.assertEqual(recipe['source_sha256'], identity['unpacked_sha256'])
        self.assertEqual(int(recipe['load_base'], 0), int(identity['load_base'], 0))
        self.assertEqual(recipe['relocation_count'], 902)
        self.assertEqual(len(recipe['operations']), 902)
        sites = set()
        counts = {}
        for row in recipe['operations']:
            self.assertEqual(set(row), {'segment', 'offset', 'mask', 'addend'})
            offset, mask = int(row['offset'], 0), int(row['mask'], 0)
            self.assertNotIn((row['segment'], offset), sites)
            sites.add((row['segment'], offset))
            self.assertEqual(offset % 4, 0)
            self.assertLessEqual(offset + 4, int(identity['snapshots'][row['segment']]['filesz'], 0))
            counts[mask] = counts.get(mask, 0) + 1
        self.assertEqual(counts, {0xffffffff: 275, 0x03ffffff: 507, 0xffff: 120})

    def test_source_identity_schema_base_and_count_fail_closed(self):
        recipe = json.loads((PROFILE / 'config/font_relocations.json').read_text())
        identity = json.loads((PROFILE / 'config/font_module.json').read_text())
        with self.assertRaisesRegex(ValueError, 'source SHA256'):
            font.reconstruct(b'wrong input', recipe, identity)
        wrong = copy.deepcopy(recipe)
        wrong['original_words'] = [0]
        with self.assertRaisesRegex(ValueError, 'schema'):
            font.reconstruct(b'wrong input', wrong, identity)
        source = synthetic_elf()
        identity = {**identity, 'unpacked_sha256': hashlib.sha256(source).hexdigest()}
        recipe = {**recipe, 'source_sha256': identity['unpacked_sha256']}
        with self.assertRaisesRegex(ValueError, 'base'):
            font.reconstruct(source, {**recipe, 'load_base': '0x08C00000'}, identity)
        with self.assertRaisesRegex(ValueError, '902'):
            font.reconstruct(source, {**recipe, 'relocation_count': 901}, identity)

    def test_segment_output_hash_is_independent_of_recipe_metadata(self):
        first = struct.pack('<I', (9 << 26)) * 902
        second = bytes(4)
        source = synthetic_elf(first, second)
        layout = font.segments(source)
        identity = {'unpacked_sha256': hashlib.sha256(source).hexdigest(), 'load_base': '0x09FD0000',
            'snapshots': [{**row, 'sha256': '0' * 64} for row in layout]}
        recipe = {'schema': 1, 'algorithm': 'masked_modular_addition', 'source_sha256': identity['unpacked_sha256'],
            'load_base': '0x09FD0000', 'relocation_count': 902, 'provenance': 'synthetic test only',
            'operations': [operation(index * 4, 0xffff, 0) for index in range(902)]}
        with self.assertRaisesRegex(ValueError, 'segment SHA256'):
            font.reconstruct(source, recipe, identity)


if __name__ == '__main__':
    unittest.main()
