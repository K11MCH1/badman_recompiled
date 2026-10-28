import importlib.util
import json
import os
import re
import shutil
from pathlib import Path
import struct
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/build_game.py'
SPEC = importlib.util.spec_from_file_location('badman_build_game', SCRIPT)
build_game = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(build_game)
REAL_DISCOVER_TOOLCHAIN = build_game.discover_toolchain


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True); path.write_text(json.dumps(value), encoding='utf-8')


def executable_bytes():
    data = bytearray(4096); data[:2] = b'MZ'; struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'; struct.pack_into('<HH', data, 132, 0x8664, 1)
    struct.pack_into('<HH', data, 148, 240, 2); struct.pack_into('<H', data, 152, 0x20B)
    struct.pack_into('<I', data, 168, 0x1000); struct.pack_into('<I', data, 208, 0x2000)
    return data


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='badman-build-tests-')
        self.root = Path(self.temp.name); self.source = self.root / 'runtime-source'; self.data = self.root / '사용자 게임 data'
        self.profile = self.source / 'profiles/badman'; self.data.mkdir()
        for relative in ['CMakeLists.txt', 'tools/codegen_main.cpp', 'tools/windows.manifest', 'profiles/badman/CMakeLists.txt',
                         'profiles/badman/host/main.cpp', 'profiles/badman/config/code_ranges.csv', 'profiles/badman/config/font_code_ranges.csv']:
            path = self.source / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('// independent fixture\n')
        for filename, count in [('generate.py', 66), ('generate_font.py', 4)]:
            path = self.profile / 'tools' / filename; path.parent.mkdir(exist_ok=True)
            path.write_text(f'def groups(*args, **kwargs):\n    return [None] * {count}\n', encoding='utf-8')
        self.elf = self.data / 'main.elf'; self.font = self.data / 'font.elf'; self.elf.write_bytes(b'\x7fELFmain'); self.font.write_bytes(b'\x7fELFfont')
        write_json(self.profile / 'config/executable.json', {'elf_sha256': build_game.sha(self.elf)})
        write_json(self.profile / 'config/font_module.json', {'prepared_sha256': build_game.sha(self.font), 'load_base': '0x09FD0000', 'imports': []})
        tool = self.root / 'fake-cmake.exe'; tool.write_bytes(b'synthetic tool identity')
        self.toolchain = {'generator': 'Visual Studio 17 2022', 'architecture': 'x64', 'instance': 'C:/VSfixture',
                          'vc_version': '14.44.35207', 'sdk_version': '10.0.26100.0',
                          'tools': {'cmake': {'path': str(tool), 'sha256': build_game.sha(tool)}}}
        self.commands = []; self.messages = []; self.fail = False; self.change_source = False
        self.discover = patch.object(build_game, 'discover_toolchain', return_value=self.toolchain); self.discover.start()
        self.ffmpeg = patch.object(build_game, 'ffmpeg_inventory', return_value={'include/header.h': 'b' * 64}); self.ffmpeg.start()
        self.run = patch.object(build_game, 'run_checked', side_effect=self.compiler); self.run.start()

    def tearDown(self):
        self.run.stop(); self.ffmpeg.stop(); self.discover.stop(); self.temp.cleanup()

    def compiler(self, command, log, emit, environment=None):
        self.commands.append([str(item) for item in command]); log.write('synthetic compiler diagnostic\n'); log.flush()
        if self.fail:
            raise build_game.BuildError('native_build_failed', 'Synthetic compiler failed')
        if '--build' not in command:
            return
        directory = Path(command[command.index('--build') + 1])
        self.assertEqual(environment['TEMP'], str(directory / '.tmp')); self.assertEqual(environment['TMP'], environment['TEMP'])
        self.assertEqual(environment['PYTHONDONTWRITEBYTECODE'], '1')
        native = directory / 'bin/Release/BadmanNative.exe'; native.parent.mkdir(parents=True)
        native.write_bytes(executable_bytes())
        recompiler = directory / 'Release/psp_recomp.exe'; recompiler.parent.mkdir(); recompiler.write_bytes(b'new locally compiled generic tool')
        for label, folder, generator, ranges, count, elf in [
            ('main', 'generated', 'generate.py', 'code_ranges.csv', 66, self.elf),
            ('font', 'font_generated', 'generate_font.py', 'font_code_ranges.csv', 4, self.font)]:
            target = directory / 'profiles/badman' / folder; target.mkdir(parents=True)
            names = [f'{label}_{i}.cpp' for i in range(count + 2)]
            for name in names:
                (target / name).write_text('// synthetic translation created by compiler fixture\n')
            write_json(target / ('generation.json' if label == 'main' else 'font_generation.json'),
                {'translation_units': count, 'sources': names, 'inputs': {'elf': build_game.sha(elf), 'recompiler': build_game.sha(recompiler),
                 'generator': build_game.sha(self.profile / 'tools' / generator), 'ranges': build_game.sha(self.profile / 'config' / ranges)}})
        if self.change_source:
            (self.profile / 'host/main.cpp').write_text('// source changed during compilation')

    def ensure(self, **kwargs):
        return build_game.ensure_build(self.data, self.elf, self.font, source_root=self.source,
                                      ffmpeg_prefix=self.root / 'ffmpeg', emit=self.messages.append, **kwargs)

    def test_fresh_build_generates_bounded_local_corpus_and_verifies_cache(self):
        executable = self.ensure()
        self.assertTrue(executable.is_relative_to(self.data / 'build')); self.assertEqual(len(self.commands), 2)
        configure, compile = self.commands
        self.assertIn('-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF', configure)
        self.assertIn('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', configure)
        self.assertIn('-DBADMAN_ELF=' + self.elf.as_posix(), configure)
        self.assertIn('-DBADMAN_FONT_ELF=' + self.font.as_posix(), configure)
        self.assertEqual(compile[-4:], ['--target', 'BadmanNative', '--parallel', '1'])
        descriptor = build_game.read_build_descriptor(self.data); self.assertEqual(descriptor['sha256'], build_game.sha(executable))
        manifest = json.loads(Path(descriptor['entry_manifest']).read_text(encoding='utf-8'))
        self.assertEqual(manifest['status'], 'ready'); self.assertEqual(manifest['generation']['main_units'], 66)
        self.assertEqual(manifest['generation']['font_units'], 4); self.assertEqual(len(manifest['generation']['outputs']), 76)
        self.assertEqual(self.ensure(), executable); self.assertEqual(len(self.commands), 2)
        self.assertFalse((self.source / 'out').exists())
        self.assertFalse(list(self.source.rglob('__pycache__')))

    def test_verify_only_is_read_only_and_missing_build_stays_missing(self):
        with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
        self.assertFalse((self.data / 'build').exists()); self.assertFalse(self.commands)
        executable = self.ensure()
        files = {path: (path.stat().st_mtime_ns, build_game.sha(path)) for path in (self.data / 'build').rglob('*') if path.is_file()}
        self.assertEqual(self.ensure(verify_only=True), executable)
        self.assertEqual(files, {path: (path.stat().st_mtime_ns, build_game.sha(path)) for path in files})
        self.assertEqual(len(self.commands), 2)

    def test_source_and_toolchain_and_flags_invalidate_cache(self):
        old = self.ensure(); (self.profile / 'host/main.cpp').write_text('// revised source')
        with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
        revised = self.ensure(); self.assertNotEqual(old, revised)
        Path(self.toolchain['tools']['cmake']['path']).write_bytes(b'changed compiler identity')
        self.toolchain['tools']['cmake']['sha256'] = build_game.sha(Path(self.toolchain['tools']['cmake']['path']))
        with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
        compiler_changed = self.ensure(); self.assertNotEqual(revised, compiler_changed)
        flags_changed = self.ensure(jobs=1); self.assertNotEqual(compiler_changed, flags_changed)
        self.assertTrue(old.is_file())

    def add_vulkan_header_fixture(self):
        repo = SCRIPT.parents[3]
        for name in build_game.VULKAN_HEADERS_FILES:
            target = self.source / 'third_party/vulkan_headers' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(repo / 'third_party/vulkan_headers' / name, target)
        return self.source / 'third_party/vulkan_headers'

    def test_vulkan_header_and_host_shader_changes_invalidate_build_fingerprint(self):
        vendor = self.add_vulkan_header_fixture()
        shader = self.profile / 'host/independent.hlsl'; shader.write_text('// shader version one')
        inventory = build_game.source_inventory(self.source)
        for name in build_game.VULKAN_HEADERS_FILES:
            self.assertIn('third_party/vulkan_headers/' + name, inventory)
        self.assertIn('profiles/badman/host/independent.hlsl', inventory)
        old = self.ensure()
        header = vendor / 'include/vulkan/vulkan_core.h'; header.write_bytes(header.read_bytes() + b'\n// developer change\n')
        with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
        changed = self.ensure(); self.assertNotEqual(changed, old)
        shader.write_text('// shader version two')
        with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
        self.assertNotEqual(self.ensure(), changed)
        self.assertTrue(old.is_file())

    def test_vulkan_closure_missing_or_arbitrary_files_are_rejected(self):
        vendor = self.add_vulkan_header_fixture()
        extra = vendor / 'include/vulkan/unreviewed.h'; extra.write_text('// not approved')
        with self.assertRaises(build_game.BuildError): build_game.source_inventory(self.source)
        extra.unlink()
        license = vendor / 'LICENSES/Apache-2.0.txt'; license.unlink()
        with self.assertRaises(build_game.BuildError): build_game.source_inventory(self.source)

    def test_packaged_build_manifest_must_cover_every_vulkan_header(self):
        self.add_vulkan_header_fixture()
        inventory = build_game.source_inventory(self.source)
        omitted = 'third_party/vulkan_headers/include/vulkan/vulkan_core.h'
        entries = [{'path': name, 'sha256': value, 'bytes': (self.source / name).stat().st_size}
                   for name, value in inventory.items() if name != omitted]
        write_json(self.source / 'source-manifest.json', {'schema': 1, 'game_independent_source_only': True, 'files': entries})
        with self.assertRaises(build_game.BuildError) as error: build_game.source_inventory(self.source)
        self.assertEqual(error.exception.reason, 'runtime_source_changed')

    def test_tampered_executable_forces_fresh_tree_not_up_to_date_reuse(self):
        executable = self.ensure(); executable.write_bytes(executable_bytes() + b'tampered')
        with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
        entry = executable.parents[3]
        self.assertEqual(self.ensure(), executable); self.assertEqual(len(self.commands), 4)
        preserved = list(entry.glob('previous-*')); self.assertEqual(len(preserved), 1)
        self.assertTrue((preserved[0] / 'bin/Release/BadmanNative.exe').read_bytes().endswith(b'tampered'))

    def test_compiler_environment_flags_invalidate_cache(self):
        old = self.ensure()
        with patch.dict(os.environ, {'CL': '/DLOCAL_BUILD_FLAG=1'}):
            with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
            self.assertNotEqual(old, self.ensure())

    def test_tampered_translation_rejected_by_readonly_verification(self):
        executable = self.ensure(); directory = executable.parents[2]
        source = next((directory / 'profiles/badman/generated').glob('*.cpp')); source.write_text('// altered translation')
        with self.assertRaises(build_game.BuildError): self.ensure(verify_only=True)
        self.ensure(); self.assertEqual(len(self.commands), 4)

    def test_mismatched_game_input_never_writes_build_or_uses_prebuilt_fallback(self):
        prebuilt = self.source / 'out/badman/bin/Release/BadmanNative.exe'; prebuilt.parent.mkdir(parents=True); prebuilt.write_bytes(executable_bytes())
        self.elf.write_bytes(b'wrong ELF')
        with self.assertRaises(build_game.BuildError) as error: self.ensure()
        self.assertEqual(error.exception.reason, 'unsupported_build_input'); self.assertFalse((self.data / 'build').exists())
        self.assertFalse(self.commands)

    def test_unowned_nonempty_build_directory_is_preserved(self):
        directory = self.data / 'build'; directory.mkdir(); unrelated = directory / 'user.txt'; unrelated.write_text('keep')
        with self.assertRaises(build_game.BuildError) as error: self.ensure()
        self.assertEqual(error.exception.reason, 'occupied_build_directory'); self.assertEqual(unrelated.read_text(), 'keep')
        self.assertEqual(list(directory.iterdir()), [unrelated]); self.assertFalse(self.commands)

    def test_failed_compilation_keeps_diagnostics_and_never_publishes_ready(self):
        self.fail = True
        with self.assertRaises(build_game.BuildError): self.ensure()
        manifests = list((self.data / 'build').glob('*/build.json')); self.assertEqual(len(manifests), 1)
        manifest = json.loads(manifests[0].read_text(encoding='utf-8')); self.assertEqual(manifest['status'], 'failed')
        self.assertIn('synthetic compiler diagnostic', Path(manifest['log']).read_text())
        self.assertFalse(build_game.build_cache_manifest_path(self.data).exists())

    def test_source_changed_during_build_cannot_become_ready(self):
        self.change_source = True
        with self.assertRaises(build_game.BuildError) as error: self.ensure()
        self.assertEqual(error.exception.reason, 'build_inputs_changed')
        self.assertFalse(build_game.build_cache_manifest_path(self.data).exists())

    def test_packaged_source_manifest_verified_and_not_trusted_by_name(self):
        files = []
        for path in self.source.rglob('*'):
            if path.is_file(): files.append({'path': path.relative_to(self.source).as_posix(), 'bytes': path.stat().st_size, 'sha256': build_game.sha(path)})
        write_json(self.source / 'source-manifest.json', {'schema': 1, 'game_independent_source_only': True, 'files': files})
        self.ensure(); (self.profile / 'host/main.cpp').write_text('modified packaged source')
        with self.assertRaises(build_game.BuildError) as error: self.ensure()
        self.assertEqual(error.exception.reason, 'runtime_source_changed')

    def test_invalid_native_artifacts_and_incomplete_sdk_are_rejected(self):
        path = self.root / 'invalid.exe'; path.write_bytes(b'MZ incomplete')
        with self.assertRaises(build_game.BuildError): build_game.pe_check(path)
        path.write_bytes(executable_bytes()); build_game.pe_check(path)
        self.ffmpeg.stop()
        with self.assertRaises(build_game.BuildError) as error: build_game.ffmpeg_inventory(self.root / 'missing-sdk')
        self.assertEqual(error.exception.reason, 'missing_ffmpeg_sdk')
        self.ffmpeg.start()

    def test_translation_partition_limit_rejected_before_compilation(self):
        (self.profile / 'tools/generate.py').write_text('def groups(*args, **kwargs):\n    return [None]*71\n')
        with self.assertRaises(build_game.BuildError) as error: self.ensure()
        self.assertEqual(error.exception.reason, 'invalid_translation_layout'); self.assertFalse(self.commands)


class CmakeUnicodeTests(unittest.TestCase):
    def test_actual_cmake_generator_list_blocks_decode_unicode_paths(self):
        # Exercise the actual profile execute_process blocks, not a copied
        # encoding setting. This caught CP437 decoding despite successful file
        # generation in the real Unicode build. No commercial input is needed.
        try:
            cmake = REAL_DISCOVER_TOOLCHAIN()['tools']['cmake']['path']
        except build_game.BuildError as error:
            self.skipTest('Local Windows CMake toolchain unavailable: ' + str(error))
        profile = SCRIPT.parents[1]
        text = (profile / 'CMakeLists.txt').read_text(encoding='utf-8')
        blocks = re.findall(r'execute_process\(COMMAND "\$\{Python3_EXECUTABLE\}".*?COMMAND_ERROR_IS_FATAL ANY\)', text, re.DOTALL)
        self.assertEqual(len(blocks), 2)
        with tempfile.TemporaryDirectory(prefix='badman-cmake-list-') as directory:
            root = Path(directory); main = root / '한글 main'; font = root / '글꼴 font'; output = root / 'paths.txt'
            quote = lambda path: '[==[' + Path(path).as_posix() + ']==]'
            code = '\n'.join(['cmake_minimum_required(VERSION 3.20)',
                'set(Python3_EXECUTABLE ' + quote(sys.executable) + ')',
                'set(CMAKE_BINARY_DIR ' + quote(root) + ')',
                'set(BADMAN_GENERATOR ' + quote(profile / 'tools/generate.py') + ')',
                'set(BADMAN_FONT_GENERATOR ' + quote(profile / 'tools/generate_font.py') + ')',
                'set(BADMAN_GENERATED ' + quote(main) + ')', 'set(BADMAN_FONT_GENERATED ' + quote(font) + ')',
                *blocks, 'file(WRITE ' + quote(output) + ' "${BADMAN_SOURCES}\\n${BADMAN_FONT_SOURCES}")'])
            script = root / 'probe.cmake'; script.write_text(code, encoding='utf-8')
            environment = dict(os.environ); environment.pop('PYTHONUTF8', None); environment.pop('PYTHONIOENCODING', None)
            process = subprocess.run([cmake, '-P', str(script)], env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     encoding='utf-8', errors='replace', creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(process.returncode, 0, process.stdout)
            main_paths, font_paths = output.read_text(encoding='utf-8').splitlines()
            self.assertEqual(len(main_paths.split(';')), 68); self.assertEqual(len(font_paths.split(';')), 6)
            self.assertTrue(all(Path(value).parent == main for value in main_paths.split(';')))
            self.assertTrue(all(Path(value).parent == font for value in font_paths.split(';')))


if __name__ == '__main__': unittest.main()
