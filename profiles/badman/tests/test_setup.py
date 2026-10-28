"""Synthetic launcher preparation tests; no ISO, commercial code, or tool install required."""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

PROFILE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('badman_setup', PROFILE / 'tools/setup_game.py')
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)


def sfo(version='1.01'):
    keys, values, records = bytearray(), bytearray(), []
    for key, value in [('DISC_ID', 'ULUS10456'), ('DISC_VERSION', version)]:
        text = value.encode() + b'\0'
        records.append(struct.pack('<HHIII', len(keys), 0x204, len(text), len(text), len(values)))
        keys.extend(key.encode() + b'\0')
        values.extend(text)
    key_start = 20 + len(records) * 16
    return struct.pack('<4sIIII', b'\0PSF', 0x101, key_start, key_start + len(keys), len(records)) + b''.join(records) + keys + values


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='badman-setup-tests-')
        self.parent = Path(self.temporary.name).resolve()
        self.iso = self.parent / 'user game & literal$(none).iso'
        self.iso.write_bytes(b'synthetic iso contents')
        self.elf, self.module, self.sfo = b'\x7fELFsynthetic-body', b'~SCEsynthetic-module', sfo()
        self.fixed_font = b'\x7fELFsynthetic-fixed-font'
        self.identity = {'disc_id': 'ULUS10456', 'disc_version': '1.01',
            'iso_size': self.iso.stat().st_size, 'iso_sha256': hashlib.sha256(self.iso.read_bytes()).hexdigest(),
            'elf_sha256': hashlib.sha256(self.elf).hexdigest()}
        self.font_identity = {'input_sha256': hashlib.sha256(self.module).hexdigest(),
                              'prepared_sha256': hashlib.sha256(self.fixed_font).hexdigest()}
        self.root = self.parent / 'data & game files'
        self.tool = self.parent / 'fake tool & name.exe'
        self.tool.write_bytes(b'synthetic test executable placeholder')
        self.native = self.root / 'build/synthetic fingerprint/bin/native & name.exe'
        self.native_bytes = b'MZsynthetic locally compiled test executable'
        self.args = argparse.Namespace(iso=self.iso, data_root=self.root, font_directory=None,
            dispatch_limit=12345, trace=False, play=False, verify_only=False,
            source_root=None, ffmpeg_prefix=None, cmake=None, jobs=2,
            sevenzip=self.tool, decryptor=self.tool)
        self.events, self.commands, self.build_calls, self.dependency_calls = [], [], [], []
        self.native_envs = []
        self.ffmpeg = self.parent / 'verified decoder dependency'

    def tearDown(self):
        # Verify exact test-owned target before TemporaryDirectory recursively cleans it.
        self.assertEqual(self.parent.parent, Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.parent.name.startswith('badman-setup-tests-'))
        self.assertEqual(Path(self.temporary.name).resolve(), self.parent)
        self.temporary.cleanup()

    def write_data(self, root, decrypted=True):
        for relative, data in [(backend.SFO, self.sfo), (backend.MODULE, self.module),
                               ('PSP_GAME/SYSDIR/EBOOT.BIN', b'~PSPsynthetic-encrypted')]:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        if decrypted:
            (root / backend.ELF).write_bytes(self.elf)

    @staticmethod
    def listing():
        return ('Path = archive.iso\nType = Iso\n----------\n' + ''.join(
            'Path = ' + name + '\nSymbolic Link = \nHard Link = \n\n' for name in
            ['PSP_GAME', backend.SFO, 'PSP_GAME/SYSDIR/EBOOT.BIN', backend.MODULE]))

    def runner(self, command, emit, capture=False, env=None):
        self.assertIsInstance(command, list)
        self.commands.append([str(value) for value in command])
        if str(command[0]) == str(self.native):
            self.native_envs.append(env)
            emit('Stop: synthetic diagnostic limit')
            return 1, ''
        if command[1] == 'l':
            self.assertTrue(capture)
            return 0, self.listing()
        if command[1] == 'x':
            stage = Path(next(str(value)[2:] for value in command if str(value).startswith('-o')))
            self.assertEqual(stage.parent, self.root.parent)
            self.assertIn('-xr!PSP_GAME/SYSDIR/UPDATE', command)
            self.write_data(stage, decrypted=False)
            return 0, ''
        if command[1] == '-o':
            Path(command[2]).write_bytes(self.elf)
            return 0, ''
        self.fail('Unexpected tool invocation: ' + repr(command))

    def run_setup(self, runner=None):
        return backend.setup(self.args, self.identity, self.font_identity, self.events.append,
                             self.runner if runner is None else runner, font_preparer=self.font_preparer,
                             builder=self.builder, dependency_preparer=self.dependencies)

    def dependencies(self, data_root, **options):
        self.assertEqual(data_root, self.root)
        self.dependency_calls.append(options)
        return {'sevenzip': self.tool, 'decryptor': self.tool, 'ffmpeg_prefix': self.ffmpeg}

    def builder(self, data_root, elf, font_elf, **options):
        self.assertEqual(data_root, self.root)
        self.assertEqual(elf.read_bytes(), self.elf)
        self.assertEqual(font_elf.read_bytes(), self.fixed_font)
        self.build_calls.append(options)
        if self.native.exists():
            if self.native.read_bytes() != self.native_bytes:
                raise backend.SetupError('native_cache_invalid', 'Synthetic build hash mismatch')
        elif options['verify_only']:
            raise backend.SetupError('native_build_not_ready', 'No verified locally compiled artifact')
        else:
            self.native.parent.mkdir(parents=True, exist_ok=True)
            self.native.write_bytes(self.native_bytes)
        return self.native

    def font_preparer(self, root, identity, decryptor, emit, runner, verify_only=False):
        # Synthetic fixture dependency; production crypto/reconstruction is
        # exercised separately with its bounded input contracts below.
        target = backend.prepared_font_path(root, identity)
        if backend.checked_fixed_font(root, target, identity):
            return target
        if verify_only:
            raise backend.SetupError('font_not_ready', 'Synthetic prepared font is not ready')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(self.fixed_font)
        return target

    def test_fresh_setup_uses_argument_lists_and_staging(self):
        self.assertEqual(self.run_setup(), 0)
        self.assertEqual(len(self.commands), 3)
        self.assertEqual(self.commands[0], [str(self.tool), 'l', '-slt', str(self.iso)])
        self.assertIn(str(self.iso), self.commands[1])
        self.assertEqual((self.root / backend.ELF).read_bytes(), self.elf)
        manifest = json.loads((self.root / backend.MANIFEST).read_text())
        self.assertEqual(manifest['state'], 'ready')
        self.assertEqual(manifest['schema'], 3)
        self.assertEqual(manifest['iso']['sha256'], self.identity['iso_sha256'])
        self.assertEqual(manifest['data']['fixed_font']['sha256'], self.font_identity['prepared_sha256'])
        self.assertEqual(manifest['data']['compiled_native']['path'], self.native.relative_to(self.root).as_posix())
        self.assertEqual(manifest['data']['compiled_native']['sha256'], hashlib.sha256(self.native_bytes).hexdigest())
        self.assertEqual(len(self.build_calls), 1)
        self.assertFalse(list(self.parent.glob('.badman-setup-*')))
        self.assertTrue(any(event.startswith('READY:') for event in self.events))

    def test_wrong_iso_hash_is_rejected_without_data_mutation(self):
        self.iso.write_bytes(b'x' * self.identity['iso_size'])
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'unsupported_iso')
        self.assertFalse(self.root.exists())
        self.assertEqual(self.commands, [])

    def test_occupied_unrelated_folder_is_preserved(self):
        self.root.mkdir()
        marker = self.root / 'unrelated.txt'
        marker.write_text('keep me')
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'occupied_data_root')
        self.assertEqual(marker.read_text(), 'keep me')
        self.assertEqual(list(self.root.iterdir()), [marker])
        self.assertEqual(self.commands, [])

    def test_valid_existing_data_fast_reuse_preserves_files(self):
        self.write_data(self.root)
        marker = self.root / 'unrelated.txt'
        marker.write_text('keep me')
        before = {p: p.stat().st_mtime_ns for p in [self.root / backend.ELF, self.root / backend.MODULE, marker]}
        self.assertEqual(self.run_setup(), 0)
        with mock.patch.object(backend, 'digest', wraps=backend.digest) as hashed:
            self.assertEqual(self.run_setup(), 0)
            self.assertNotIn(self.iso, [call.args[0] for call in hashed.call_args_list])
        self.assertEqual(before, {p: p.stat().st_mtime_ns for p in before})
        self.assertEqual(marker.read_text(), 'keep me')
        self.assertEqual(self.commands, [])

    def test_verify_only_full_hashes_source_without_writing_manifest(self):
        self.write_data(self.root)
        self.assertEqual(self.run_setup(), 0)
        manifest = self.root / backend.MANIFEST
        before = manifest.read_bytes()
        previous_stat = self.iso.stat()
        self.iso.write_bytes(b'x' * self.identity['iso_size'])
        os.utime(self.iso, ns=(previous_stat.st_atime_ns, previous_stat.st_mtime_ns))
        self.args.verify_only = True
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'unsupported_iso')
        self.assertEqual(manifest.read_bytes(), before)
        self.assertEqual(self.commands, [])

    def test_source_change_during_font_preparation_blocks_new_readiness(self):
        self.write_data(self.root)
        marker = self.root / 'unrelated.txt'
        marker.write_text('preserve this')
        paths = [self.root / backend.ELF, self.root / backend.MODULE, self.root / backend.SFO, marker]
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths}
        self.args.play = True
        def change_source(*args, **kwargs):
            target = self.font_preparer(*args, **kwargs)
            stat = self.iso.stat()
            self.iso.write_bytes(b'x' * self.identity['iso_size'])
            os.utime(self.iso, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10000000))
            return target
        with self.assertRaises(backend.SetupError) as caught:
            backend.setup(self.args, self.identity, self.font_identity, self.events.append,
                          self.runner, font_preparer=change_source, builder=self.builder, dependency_preparer=self.dependencies)
        self.assertEqual(caught.exception.reason, 'iso_changed')
        self.assertFalse((self.root / backend.MANIFEST).exists())
        self.assertFalse(any(event.startswith('READY:') for event in self.events))
        self.assertEqual(self.commands, [])  # No native process starts.
        self.assertEqual(before, {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths})

    def test_source_change_during_cached_font_reuse_preserves_existing_manifest(self):
        self.write_data(self.root)
        self.assertEqual(self.run_setup(), 0)
        manifest = self.root / backend.MANIFEST
        before = manifest.read_bytes()
        marker = self.root / 'unrelated.txt'
        marker.write_text('still here')
        self.events.clear()
        def change_source(*args, **kwargs):
            target = self.font_preparer(*args, **kwargs)
            stat = self.iso.stat()
            os.utime(self.iso, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10000000))
            return target
        with self.assertRaises(backend.SetupError) as caught:
            backend.setup(self.args, self.identity, self.font_identity, self.events.append,
                          self.runner, font_preparer=change_source, builder=self.builder, dependency_preparer=self.dependencies)
        self.assertEqual(caught.exception.reason, 'iso_changed')
        self.assertEqual(manifest.read_bytes(), before)
        self.assertEqual(marker.read_text(), 'still here')
        self.assertFalse(any(event.startswith('READY:') for event in self.events))
        self.assertEqual(self.commands, [])

    def test_bad_extracted_identity_is_preserved_not_repaired_in_place(self):
        self.write_data(self.root)
        (self.root / backend.ELF).write_bytes(b'\x7fELFwrong body')
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'occupied_data_root')
        self.assertEqual((self.root / backend.ELF).read_bytes(), b'\x7fELFwrong body')
        self.assertEqual(self.commands, [])

    def test_disc_version_and_sfo_bounds_are_checked(self):
        self.write_data(self.root)
        (self.root / backend.SFO).write_bytes(sfo('9.99'))
        with self.assertRaises(backend.SetupError):
            self.run_setup()
        with self.assertRaises(backend.SetupError):
            backend.parse_sfo(b'\0PSF')

    def test_unrecognized_manifest_is_not_overwritten(self):
        self.write_data(self.root)
        (self.root / backend.MANIFEST).write_text('unrelated user document')
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'occupied_data_root')
        self.assertEqual((self.root / backend.MANIFEST).read_text(), 'unrelated user document')

    def test_listing_rejects_traversal_links_and_duplicate_destinations(self):
        for entry in ['PSP_GAME/../escape', '/absolute', 'C:/absolute']:
            with self.subTest(entry=entry), self.assertRaises(backend.SetupError):
                backend.validate_archive_listing(self.listing() + 'Path = ' + entry + '\n')
        with self.assertRaises(backend.SetupError):
            backend.validate_archive_listing(self.listing() + 'Symbolic Link = ../escape\n')
        with self.assertRaises(backend.SetupError):
            backend.validate_archive_listing(self.listing() + 'Path = psp_game/param.sfo\nPath = PSP_GAME/PARAM.SFO\n')

    def test_failed_decrypt_cleans_only_its_stage(self):
        def failure(command, emit, capture=False):
            if command[1] == '-o':
                return 7, ''
            return self.runner(command, emit, capture)
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup(failure)
        self.assertEqual(caught.exception.reason, 'decryption_failed')
        self.assertFalse(self.root.exists())
        self.assertFalse(list(self.parent.glob('.badman-setup-*')))
        self.assertTrue(self.iso.exists())

    def test_play_exact_arguments_and_native_failure_are_reported(self):
        self.write_data(self.root)
        fonts = self.parent / 'font files & names'
        fonts.mkdir()
        (fonts / 'synthetic.pgf').write_bytes(b'synthetic fixture')
        self.args.font_directory, self.args.play, self.args.trace = fonts, True, True
        self.assertEqual(self.run_setup(), 1)
        command = self.commands[-1]
        self.assertEqual(command[:4], [str(self.native), str(self.root / backend.ELF), str(self.root), '12345'])
        self.assertTrue(command[4].endswith('-hle.log'))
        self.assertEqual(command[5], str(fonts))
        self.assertEqual(command[6], str(backend.prepared_font_path(self.root, self.font_identity)))
        self.assertTrue(any('Native session stopped (exit 1)' in line for line in self.events))
        logs = list((self.root / 'launcher-logs').glob('*-session.log'))
        self.assertEqual(len(logs), 1)
        self.assertIn('synthetic diagnostic limit', logs[0].read_text())

    def test_renderer_parser_default_choices_and_invalid_choice(self):
        common = ['--iso', str(self.iso), '--data-root', str(self.root)]
        self.assertEqual(backend.parser().parse_args(common).renderer, 'd3d11')
        for renderer in ('d3d11', 'vulkan', 'software'):
            with self.subTest(renderer=renderer):
                self.assertEqual(backend.parser().parse_args(common + ['--renderer', renderer]).renderer, renderer)
        with mock.patch('sys.stderr', new=io.StringIO()), self.assertRaises(SystemExit) as caught:
            backend.parser().parse_args(common + ['--renderer', 'unknown'])
        self.assertEqual(caught.exception.code, 2)

    def test_legacy_setup_arguments_default_native_renderer_to_d3d11(self):
        self.write_data(self.root)
        self.args.play = True
        self.assertFalse(hasattr(self.args, 'renderer'))
        self.assertEqual(self.run_setup(), 1)
        self.assertEqual(self.native_envs[-1]['BADMAN_RENDERER'], 'd3d11')

    def test_preparation_does_not_select_a_backend_or_change_build_inputs(self):
        self.write_data(self.root)
        with mock.patch.dict(os.environ, {'BADMAN_RENDERER': 'parent-value'}):
            before = dict(os.environ)
            for renderer in ('d3d11', 'vulkan', 'software'):
                self.args.renderer = renderer
                self.assertEqual(self.run_setup(), 0)
            self.assertEqual(dict(os.environ), before)
        self.assertEqual(self.native_envs, [])
        self.assertEqual(self.build_calls[0], self.build_calls[1])
        self.assertEqual(self.build_calls[1], self.build_calls[2])
        self.assertNotIn('renderer', json.loads((self.root / backend.MANIFEST).read_text()))

    def test_invalid_renderer_stops_before_preparation(self):
        for value in ('unknown', None, ['vulkan']):
            with self.subTest(renderer=value), self.assertRaises(backend.SetupError) as caught:
                self.args.renderer = value
                self.run_setup()
            self.assertEqual(caught.exception.reason, 'invalid_renderer')
        self.assertEqual(self.commands, [])
        self.assertEqual(self.build_calls, [])
        self.assertFalse(self.root.exists())

    def test_mocked_owned_native_receives_renderer_without_global_environment_mutation(self):
        self.write_data(self.root)
        self.args.play = True
        for renderer in ('d3d11', 'vulkan', 'software'):
            with self.subTest(renderer=renderer), mock.patch.dict(os.environ,
                    {'BADMAN_RENDERER': 'inherited-backend', 'BADMAN_PARENT_FIXTURE': 'preserved'}):
                self.args.renderer = renderer
                before = dict(os.environ)
                # The synthetic executable is never started. Exercise the real
                # setup -> run_tool -> Popen boundary with an owned mock child.
                child = mock.Mock()
                child.stdout = io.StringIO(f'Renderer {renderer} unavailable: synthetic device failure\n')
                child.wait.return_value = 17
                with mock.patch.object(backend.subprocess, 'Popen', return_value=child) as launch:
                    self.assertEqual(self.run_setup(backend.run_tool), 17)
                launch.assert_called_once()
                command = launch.call_args.args[0]
                options = launch.call_args.kwargs
                self.assertEqual(command[0], str(self.native))
                self.assertFalse(options['shell'])
                self.assertIsNot(options['env'], os.environ)
                self.assertEqual(options['env']['BADMAN_RENDERER'], renderer)
                self.assertEqual(options['env']['BADMAN_PARENT_FIXTURE'], 'preserved')
                self.assertEqual(dict(os.environ), before)
                self.assertIn(f'Renderer {renderer} unavailable: synthetic device failure', self.events)
                self.assertTrue(any(f'Native session stopped (exit 17)' in x for x in self.events))
                # No automatic retry or software fallback was attempted.
                self.assertEqual(child.wait.call_count, 1)
        logs = [x.read_text() for x in (self.root / 'launcher-logs').glob('*-session.log')]
        self.assertEqual(len(logs), 3)
        for renderer in ('d3d11', 'vulkan', 'software'):
            self.assertTrue(any(f'Renderer {renderer} unavailable' in text for text in logs))

    def test_native_launch_error_names_the_selected_backend(self):
        self.write_data(self.root)
        self.args.play, self.args.renderer = True, 'vulkan'
        with mock.patch.object(backend.subprocess, 'Popen', side_effect=OSError('owned mock launch failed')):
            with self.assertRaises(backend.SetupError) as caught:
                self.run_setup(backend.run_tool)
        self.assertEqual(caught.exception.reason, 'native_launch_failed')
        self.assertIn('Vulkan', str(caught.exception))

    def test_external_output_is_streamed_without_shell_interpretation(self):
        value = 'literal & $(not a command) with spaces'
        code, _ = backend.run_tool([sys.executable, '-X', 'utf8', '-c',
            'import sys; print(sys.argv[1]); print("stderr line",file=sys.stderr)', value], self.events.append)
        self.assertEqual(code, 0)
        self.assertIn(value, self.events)
        self.assertIn('stderr line', self.events)

    def test_readonly_helper_loading_creates_no_bytecode_artifacts(self):
        helper = self.parent / 'read only helper.py'
        helper.write_text('value = 12345\n', encoding='utf-8')
        before = set(self.parent.rglob('*'))
        module = backend.load_helper(helper, 'badman_readonly_helper_fixture')
        self.assertEqual(module.value, 12345)
        self.assertEqual(before, set(self.parent.rglob('*')))

    def test_full_packaged_font_preparation_and_verify_preserve_exact_bundle_inventory(self):
        # Exercise both real nested helper loaders, all 902 relocation fields,
        # relocated metadata validation and ELF wrapping using synthetic bytes.
        # This would reproduce the two stray pyc files from packaged QA before
        # the fix; no bytecode-disable flag masks the production import paths.
        package = self.parent / 'Immutable launcher bundle'
        tools, config = package / 'tools', package / 'config'
        tools.mkdir(parents=True)
        config.mkdir()
        for name in ['setup_game.py', 'reconstruct_font.py', 'prepare_font.py']:
            (tools / name).write_bytes((PROFILE / 'tools' / name).read_bytes())
        base = 0x09fd0000
        parts = [bytearray(0x801c), bytearray(0x234)]
        for index in range(902):
            struct.pack_into('<I', parts[0], index * 4, 9 << 26)  # Synthetic ADDIU low16 field.
        struct.pack_into('<IHHBBHI', parts[0], 0x7944, 0, 0, 0, 9, 0, 0, 0)
        struct.pack_into('<IHHBBHII', parts[0], 0x7970, 0, 0, 0, 10, 0, 0, 0, 0)
        struct.pack_into('<HH28sIIIII', parts[0], 0x799c, 6, 0x101, b'synthetic_font',
                         base + 0x1000, base + 0x7944, base + 0x7968, base + 0x7970, base + 0x7998)
        layout = [dict(vaddr='0x0', filesz='0x801c', memsz='0x801c', flags=5),
                  dict(vaddr='0x8020', filesz='0x178', memsz='0x234', flags=6)]
        unpacked = bytearray(35302)
        ident = b'\x7fELF\x01\x01\x01' + bytes(9)
        struct.pack_into('<16sHHIIIIIHHHHHH', unpacked, 0, ident, 0xffa0, 8, 1, 0xffffffff,
                         52, 0, 0, 52, 32, 2, 0, 0, 0)
        for index, offset in enumerate([0x100, 0x8120]):
            row = layout[index]
            filesz, memsz = int(row['filesz'], 0), int(row['memsz'], 0)
            struct.pack_into('<8I', unpacked, 52 + index * 32, 1, offset, int(row['vaddr'], 0),
                             0, filesz, memsz, row['flags'], 16)
            unpacked[offset:offset + filesz] = parts[index][:filesz]
        unpacked = bytes(unpacked)
        raw = b'~SCE' + struct.pack('<I', 64) + bytes(56) + b'~PSPsynthetic encrypted marker'
        identity = {'module': 'synthetic_font', 'load_base': hex(base), 'gp': hex(base + 0x1000),
            'entry': '0xFFFFFFFF', 'module_info_relative': '0x799c', 'exports': [], 'imports': [],
            'input_sha256': hashlib.sha256(raw).hexdigest(),
            'unpacked_sha256': hashlib.sha256(unpacked).hexdigest(),
            'snapshots': [{**row, 'sha256': hashlib.sha256(part).hexdigest()} for row, part in zip(layout, parts)]}
        normalizer = backend.load_helper(PROFILE / 'tools/prepare_font.py', 'badman_inventory_fixture_normalizer')
        normalizer.validate_tables(parts, identity)
        expected = normalizer.normalize(parts, identity)
        identity['prepared_sha256'] = hashlib.sha256(expected).hexdigest()
        recipe = {'schema': 1, 'algorithm': 'masked_modular_addition', 'load_base': hex(base),
            'source_sha256': identity['unpacked_sha256'], 'relocation_count': 902,
            'provenance': 'Synthetic bundle immutability regression; no commercial bytes',
            'operations': [{'segment': 0, 'offset': index * 4, 'mask': '0xffff', 'addend': 0} for index in range(902)]}
        (config / 'font_module.json').write_text(json.dumps(identity), encoding='utf-8')
        (config / 'font_relocations.json').write_text(json.dumps(recipe), encoding='utf-8')
        self.write_data(self.root)
        (self.root / backend.MODULE).write_bytes(raw)
        def inventory():
            return {path.relative_to(package).as_posix():
                    ('file', path.stat().st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest()) if path.is_file()
                    else ('directory', path.stat().st_mtime_ns) for path in package.rglob('*')}
        before = inventory()
        packaged = backend.load_helper(tools / 'setup_game.py', 'badman_packaged_font_inventory_fixture')
        calls = []
        def decrypt(command, emit, capture=False):
            calls.append(command)
            self.assertEqual(Path(command[3]).read_bytes(), raw[64:])
            Path(command[2]).write_bytes(gzip.compress(unpacked))
            return 0, ''
        target = packaged.prepare_font_module(self.root, identity, self.tool, self.events.append, decrypt)
        self.assertEqual(target.read_bytes(), expected)
        self.assertEqual(len(calls), 1)
        self.assertEqual(inventory(), before)
        def no_tool(*args, **kwargs):
            self.fail('Read-only prepared font verification must not invoke a decryptor')
        self.assertEqual(packaged.prepare_font_module(self.root, identity, self.tool,
            self.events.append, no_tool, verify_only=True), target)
        self.assertEqual(inventory(), before)
        self.assertFalse(list(package.rglob('*.pyc')))

    def test_verify_only_requires_prepared_font_without_writes(self):
        self.write_data(self.root)
        self.args.verify_only = True
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'font_not_ready')
        self.assertFalse((self.root / 'prepared').exists())
        self.assertFalse((self.root / backend.MANIFEST).exists())

    def test_missing_font_directory_slot_is_explicitly_empty_before_font_elf(self):
        self.write_data(self.root)
        self.args.play = True
        self.assertEqual(self.run_setup(), 1)
        self.assertEqual(self.commands[-1][5], '')
        self.assertEqual(self.commands[-1][6], str(backend.prepared_font_path(self.root, self.font_identity)))

    def test_verify_only_requires_local_build_and_never_uses_prebuilt_fallback(self):
        self.write_data(self.root)
        self.font_preparer(self.root, self.font_identity, self.tool, self.events.append, self.runner)
        prebuilt = self.parent / 'BadmanNative.exe'
        prebuilt.write_bytes(b'MZunrelated prebuilt executable')
        self.args.native = prebuilt  # Old caller fields do not authorize this artifact.
        self.args.verify_only = True
        before = {path.relative_to(self.root): path.read_bytes() for path in self.root.rglob('*') if path.is_file()}
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'native_build_not_ready')
        self.assertEqual(before, {path.relative_to(self.root): path.read_bytes() for path in self.root.rglob('*') if path.is_file()})
        self.assertFalse(self.native.exists())
        self.assertEqual(prebuilt.read_bytes(), b'MZunrelated prebuilt executable')
        self.assertTrue(self.build_calls[-1]['verify_only'])

    def test_compiler_failure_preserves_previous_readiness_and_unrelated_data(self):
        self.write_data(self.root)
        self.run_setup()
        manifest = self.root / backend.MANIFEST
        previous = manifest.read_bytes()
        marker = self.root / 'unrelated.txt'
        marker.write_text('preserve existing files')
        self.events.clear()
        self.args.play = True
        def fail(*args, **options):
            raise backend.SetupError('compiler_missing', 'Install the supported compiler before building')
        with self.assertRaises(backend.SetupError) as caught:
            backend.setup(self.args, self.identity, self.font_identity, self.events.append, self.runner,
                          font_preparer=self.font_preparer, builder=fail, dependency_preparer=self.dependencies)
        self.assertEqual(caught.exception.reason, 'compiler_missing')
        self.assertEqual(manifest.read_bytes(), previous)
        self.assertEqual(marker.read_text(), 'preserve existing files')
        self.assertFalse(any(event.startswith('READY:') for event in self.events))
        self.assertEqual(self.commands, [])

    def test_source_change_during_local_build_never_publishes_ready(self):
        self.write_data(self.root)
        marker = self.root / 'unrelated.txt'
        marker.write_text('keep raw data')
        before = (self.root / backend.ELF).read_bytes()
        self.args.play = True
        def changed_source(*args, **options):
            artifact = self.builder(*args, **options)
            stat = self.iso.stat()
            os.utime(self.iso, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10000000))
            return artifact
        with self.assertRaises(backend.SetupError) as caught:
            backend.setup(self.args, self.identity, self.font_identity, self.events.append, self.runner,
                          font_preparer=self.font_preparer, builder=changed_source, dependency_preparer=self.dependencies)
        self.assertEqual(caught.exception.reason, 'iso_changed')
        self.assertFalse((self.root / backend.MANIFEST).exists())
        self.assertFalse(any(event.startswith('READY:') for event in self.events))
        self.assertEqual(self.commands, [])
        self.assertEqual(marker.read_text(), 'keep raw data')
        self.assertEqual((self.root / backend.ELF).read_bytes(), before)

    def test_builder_cannot_return_executable_outside_owned_local_build(self):
        self.write_data(self.root)
        prebuilt = self.parent / 'unrelated executable.exe'
        prebuilt.write_bytes(b'MZpreserve this artifact')
        def outside(*args, **options):
            return prebuilt
        with self.assertRaises(backend.SetupError) as caught:
            backend.setup(self.args, self.identity, self.font_identity, self.events.append, self.runner,
                          font_preparer=self.font_preparer, builder=outside, dependency_preparer=self.dependencies)
        self.assertEqual(caught.exception.reason, 'unsafe_native_build')
        self.assertEqual(prebuilt.read_bytes(), b'MZpreserve this artifact')
        self.assertFalse((self.root / backend.MANIFEST).exists())

    def test_verify_only_detects_tampered_local_build_without_rebuilding(self):
        self.write_data(self.root)
        self.run_setup()
        manifest = (self.root / backend.MANIFEST).read_bytes()
        self.native.write_bytes(b'MZmodified compiled artifact')
        self.args.verify_only = True
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'native_cache_invalid')
        self.assertEqual(self.native.read_bytes(), b'MZmodified compiled artifact')
        self.assertEqual((self.root / backend.MANIFEST).read_bytes(), manifest)

    def test_valid_verify_only_preserves_native_and_manifest_timestamps(self):
        self.write_data(self.root)
        self.run_setup()
        paths = [self.native, self.root / backend.MANIFEST]
        before = {path: (path.stat().st_mtime_ns, path.read_bytes()) for path in paths}
        self.args.verify_only = True
        self.assertEqual(self.run_setup(), 0)
        self.assertEqual(before, {path: (path.stat().st_mtime_ns, path.read_bytes()) for path in paths})
        self.assertTrue(self.dependency_calls[-1]['verify_only'])
        self.assertEqual(self.build_calls[-1]['ffmpeg_prefix'], self.ffmpeg)

    def owned_tools(self):
        dependency = backend.dependency_module()
        folder = self.root / 'tools'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / dependency.OWNER).write_text(json.dumps({'producer': dependency.PRODUCER, 'schema': 1}))
        self.assertTrue(backend.owns_dependencies(self.root))
        return folder

    def test_fresh_dependency_cache_coexists_with_staged_game_install(self):
        def owned_dependencies(root, **options):
            self.owned_tools()
            return self.dependencies(root, **options)
        self.assertEqual(backend.setup(self.args, self.identity, self.font_identity, self.events.append,
            self.runner, font_preparer=self.font_preparer, builder=self.builder,
            dependency_preparer=owned_dependencies), 0)
        self.assertTrue(backend.owns_dependencies(self.root))
        self.assertEqual((self.root / backend.ELF).read_bytes(), self.elf)
        self.assertTrue(self.native.is_file())

    def test_owned_partial_dependency_cache_resumes_without_fake_game_readiness(self):
        tools = self.owned_tools()
        marker = next(tools.iterdir())
        before = marker.read_bytes()
        self.assertFalse((self.root / backend.MANIFEST).exists())
        self.assertEqual(self.run_setup(), 0)
        self.assertEqual(marker.read_bytes(), before)
        self.assertEqual((self.root / backend.ELF).read_bytes(), self.elf)

    def test_unowned_tools_folder_is_preserved_and_never_adopted(self):
        tools = self.root / 'tools'
        tools.mkdir(parents=True)
        marker = tools / 'user-file.txt'
        marker.write_text('unrelated tools')
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'occupied_data_root')
        self.assertEqual(marker.read_text(), 'unrelated tools')
        self.assertEqual(self.dependency_calls, [])

    def test_dependency_failure_never_starts_extraction_build_or_play(self):
        self.args.play = True
        def failed_dependencies(*args, **options):
            self.owned_tools()
            raise backend.SetupError('dependency_download_mismatch', 'Pinned dependency hash mismatch')
        with self.assertRaises(backend.SetupError) as caught:
            backend.setup(self.args, self.identity, self.font_identity, self.events.append,
                self.runner, font_preparer=self.font_preparer, builder=self.builder,
                dependency_preparer=failed_dependencies)
        self.assertEqual(caught.exception.reason, 'dependency_download_mismatch')
        self.assertEqual(self.commands, [])
        self.assertEqual(self.build_calls, [])
        self.assertFalse((self.root / backend.MANIFEST).exists())
        self.assertTrue(backend.owns_dependencies(self.root))

    def test_unrelated_prepared_artifact_is_preserved(self):
        self.write_data(self.root)
        target = backend.prepared_font_path(self.root, self.font_identity)
        target.parent.mkdir(parents=True)
        target.write_bytes(b'unrelated file')
        with self.assertRaises(backend.SetupError) as caught:
            self.run_setup()
        self.assertEqual(caught.exception.reason, 'occupied_prepared_font')
        self.assertEqual(target.read_bytes(), b'unrelated file')

    def test_production_wrapper_and_bounded_gzip_pipeline_with_synthetic_payload(self):
        self.write_data(self.root)
        raw = b'~SCE' + struct.pack('<I', 64) + bytes(56) + b'~PSPsynthetic encrypted font'
        (self.root / backend.MODULE).write_bytes(raw)
        unpacked = b'\x7fELF' + bytes(35302 - 4)
        identity = {**self.font_identity, 'input_sha256': hashlib.sha256(raw).hexdigest(),
                    'unpacked_sha256': hashlib.sha256(unpacked).hexdigest()}
        def decrypt(command, emit, capture=False):
            self.assertEqual(Path(command[3]).read_bytes(), raw[64:])
            Path(command[2]).write_bytes(gzip.compress(unpacked))
            return 0, ''
        def reconstruct(source, output):
            self.assertEqual(source.read_bytes(), unpacked)
            output.write_bytes(self.fixed_font)
        target = backend.prepare_font_module(self.root, identity, self.tool, self.events.append,
                                             decrypt, reconstructor=reconstruct)
        self.assertEqual(target.read_bytes(), self.fixed_font)
        self.assertEqual((self.root / backend.MODULE).read_bytes(), raw)
        self.assertFalse(list(self.parent.glob('.badman-setup-font-*')))
        before = target.stat().st_mtime_ns
        def unused(*args, **kwargs):
            self.fail('Matching prepared artifact should not invoke external tools')
        self.assertEqual(backend.prepare_font_module(self.root, identity, self.tool, self.events.append,
                                                   unused, verify_only=True), target)
        self.assertEqual(target.stat().st_mtime_ns, before)

    def test_gzip_expansion_above_exact_size_is_rejected_before_reconstruction(self):
        self.write_data(self.root)
        raw = b'~SCE' + struct.pack('<I', 64) + bytes(56) + b'~PSPsynthetic encrypted font'
        (self.root / backend.MODULE).write_bytes(raw)
        identity = {**self.font_identity, 'input_sha256': hashlib.sha256(raw).hexdigest(), 'unpacked_sha256': '0' * 64}
        def decrypt(command, emit, capture=False):
            Path(command[2]).write_bytes(gzip.compress(b'\x7fELF' + bytes(40000)))
            return 0, ''
        def unused(*args):
            self.fail('Oversized font expansion reached reconstruction')
        with self.assertRaises(backend.SetupError) as caught:
            backend.prepare_font_module(self.root, identity, self.tool, self.events.append, decrypt, reconstructor=unused)
        self.assertEqual(caught.exception.reason, 'font_unpack_failed')
        self.assertFalse((self.root / 'prepared').exists())
        self.assertFalse(list(self.parent.glob('.badman-setup-font-*')))

    def test_packaged_profile_paths_and_utf8_decryptor_preference(self):
        package = self.parent / 'Portable Player'
        tools = package / 'tools'
        tools.mkdir(parents=True)
        (tools / 'setup_game.py').write_bytes((PROFILE / 'tools/setup_game.py').read_bytes())
        for path in [package / 'BadmanNative.exe', tools / '7zip/7z.exe',
                     tools / 'pspdecrypt/pspdecrypt.exe', tools / 'pspdecrypt/pspdecrypt-utf8.exe']:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic executable fixture')
        (package / 'assets/fonts').mkdir(parents=True)
        spec = importlib.util.spec_from_file_location('packaged_badman_setup_fixture', tools / 'setup_game.py')
        packaged = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(packaged)
        self.assertEqual(packaged.PROFILE, package)
        self.assertEqual(packaged.ROOT, package)
        args = packaged.parser().parse_args(['--iso', str(self.iso), '--data-root', str(self.root)])
        self.assertFalse(hasattr(args, 'native'))
        self.assertIsNone(args.sevenzip)
        self.assertIsNone(args.decryptor)
        self.assertEqual(args.font_directory, package / 'assets/fonts')


class TitleArtTests(unittest.TestCase):
    """The launcher background is optional work after READY, from the player's own build."""
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='badman-title-art-tests-')
        self.parent = Path(self.temporary.name).resolve()
        self.art, self.messages = self.parent / 'art', []

    def tearDown(self):
        self.temporary.cleanup()

    def capture(self, runner):
        backend.prepare_title_art(self.art, self.parent / 'BadmanNative.exe', self.parent / 'game', None,
                                  self.parent / 'font.elf', {'saves': str(self.parent / 'Saves')}, 'vulkan',
                                  self.messages.append, runner)

    def test_existing_background_is_kept_without_running_the_game(self):
        self.art.mkdir(); (self.art / 'play.png').write_bytes(b'existing')
        self.capture(mock.Mock(side_effect=AssertionError('game must not run')))
        self.assertEqual((self.art / 'play.png').read_bytes(), b'existing')
        self.assertEqual(self.messages, [])

    def test_headless_neutral_capture_publishes_converted_frame(self):
        seen = {}
        def runner(command, emit, env):
            seen.update(env=env, control=Path(env['BADMAN_INPUT_CONTROL_FILE']).read_text(encoding='ascii'))
            Path(env['BADMAN_DUMP_FRAME']).write_bytes(b'frame')
            return 0, None
        converter = mock.Mock(title_art=lambda frame, target: Path(target).write_bytes(b'png:' + Path(frame).read_bytes()))
        with mock.patch.object(backend, 'load_helper', return_value=converter):
            self.capture(runner)
        self.assertEqual((self.art / 'play.png').read_bytes(), b'png:frame')
        self.assertEqual(seen['control'], '0 0 0\n')
        self.assertEqual({key: seen['env'][key] for key in ('BADMAN_HEADLESS', 'BADMAN_SKIP_INTRO', 'BADMAN_RENDERER', 'BADMAN_SAVES_DIRECTORY')},
                         {'BADMAN_HEADLESS': '1', 'BADMAN_SKIP_INTRO': '1', 'BADMAN_RENDERER': 'vulkan',
                          'BADMAN_SAVES_DIRECTORY': str(self.parent / 'Saves')})
        self.assertIn('Launcher title background prepared from your own game.', self.messages)

    def test_failed_capture_is_reported_and_never_fails_preparation(self):
        for runner in (mock.Mock(side_effect=OSError('cannot start')), mock.Mock(return_value=(1, None))):
            self.messages.clear()
            self.capture(runner)
            self.assertFalse((self.art / 'play.png').exists())
            self.assertTrue(self.messages[-1].startswith('Launcher title background was not prepared'))


if __name__ == '__main__':
    unittest.main()
