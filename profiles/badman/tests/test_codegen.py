"""Input-map invariants that prevent missing/overlapping generated code."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

PROFILE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("badman_generator", PROFILE / "tools/generate.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
font_spec = importlib.util.spec_from_file_location('badman_font_generator', PROFILE / 'tools/generate_font.py')
font_generator = importlib.util.module_from_spec(font_spec)
font_spec.loader.exec_module(font_generator)


class CodeMapTests(unittest.TestCase):
    def parse(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "map.csv"
            path.write_text("name,address,size\n" + rows)
            return generator.groups(path)

    def test_bounds_and_partitioning(self):
        groups = self.parse("a,0x08804000,0x3000\nb,0x08807000,0x1000\nc,0x08808000,0x1000\n")
        self.assertEqual([len(group) for group in groups], [2, 1])
        self.assertEqual(sum(row[2] for group in groups for row in group), 0x5000)

    def test_reject_overlapping_instructions(self):
        with self.assertRaises(ValueError):
            self.parse("a,0x08804000,0x100\nb,0x08804080,0x100\n")

    def test_reject_misaligned_ranges(self):
        with self.assertRaises(ValueError):
            self.parse("a,0x08804002,0x100\n")

    def test_required_indirect_targets_are_covered(self):
        groups = generator.groups(PROFILE / "config/code_ranges.csv")
        ranges = [row for group in groups for row in group]
        for target in [0x088E8790, 0x088E88A4, 0x08912C54, 0x08913CD0]:
            self.assertTrue(any(start <= target < start + size for _, start, size in ranges), hex(target))

    def test_font_output_default_remains_ignored_out(self):
        target = font_generator.ROOT / 'out/synthetic-font-generation-guard'
        self.assertEqual(font_generator.validate_output(target), target.resolve())
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                font_generator.validate_output(Path(directory) / 'generated')

    def test_explicit_font_build_root_requires_matching_cmake_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve() / 'owned build'
            build.mkdir()
            target = build / 'profiles/badman/generated-font'
            with self.assertRaises(ValueError):
                font_generator.validate_output(target, build)
            cache = build / 'CMakeCache.txt'
            cache.write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + str(font_generator.ROOT.resolve()) + '\n'
                             'CMAKE_CACHEFILE_DIR:INTERNAL=' + str(build) + '\n', encoding='utf-8')
            self.assertEqual(font_generator.validate_output(target, build, ['badman_font_registry.cpp']), target)
            self.assertFalse(target.exists())
            for unsafe in [build, build / '../escape']:
                with self.assertRaises(ValueError):
                    font_generator.validate_output(unsafe, build)
            cache.write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + str(build) + '\n'
                             'CMAKE_CACHEFILE_DIR:INTERNAL=' + str(build) + '\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                font_generator.validate_output(target, build)

    def test_font_generation_preserves_unrelated_or_linked_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            (build / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + str(font_generator.ROOT.resolve()) + '\n'
                                               'CMAKE_CACHEFILE_DIR:INTERNAL=' + str(build) + '\n', encoding='utf-8')
            output = build / 'font'
            output.mkdir()
            target = output / 'badman_font_registry.cpp'
            target.mkdir()
            with self.assertRaises(ValueError):
                font_generator.validate_output(output, build, [target.name])
            target.rmdir()
            unrelated = build / 'unrelated.txt'
            unrelated.write_text('preserve unrelated data')
            os.link(unrelated, target)
            with self.assertRaises(ValueError):
                font_generator.validate_output(output, build, [target.name])
            self.assertEqual(unrelated.read_text(), 'preserve unrelated data')


if __name__ == "__main__":
    unittest.main()
