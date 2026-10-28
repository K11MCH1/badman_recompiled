"""Synthetic packager checks: no downloads, native/game binaries or GUI."""
import hashlib
import importlib.util
import json
import shutil
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

PATH = Path(__file__).resolve().parents[1] / 'tools/package_player.py'
SPEC = importlib.util.spec_from_file_location('badman_package_test_subject', PATH)
package = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = package
SPEC.loader.exec_module(package)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='badman-package-test-')
        self.root = Path(self.temporary.name)
        self.output_root = self.root / 'out'; self.output_root.mkdir()
        self.source = self.root / 'synthetic.txt'; self.source.write_bytes(b'original source')
        self.archive = self.root / 'python.zip'
        self.make_archive()

    def tearDown(self):
        self.temporary.cleanup()

    def make_archive(self, extras=None):
        with zipfile.ZipFile(self.archive, 'w') as archive:
            for name in ('python.exe', 'python313.dll', 'python313.zip', 'python313._pth', 'LICENSE.txt'):
                archive.writestr(name, b'synthetic fixture, not executable')
            for name, data in extras or []:
                archive.writestr(name, data)
        self.archive_hash = package.digest(self.archive)

    def assemble(self, name='player', entries=None, expected_hash=None):
        return package.assemble(self.output_root / name,
            entries if entries is not None else [package.Entry(self.source, 'tools/synthetic.txt', 'synthetic')],
            self.archive, self.output_root, expected_hash or self.archive_hash)

    def test_complete_manifest_and_isolated_python_paths(self):
        manifest = self.assemble('한글 player')
        output = self.output_root / '한글 player'
        self.assertEqual((output / 'tools/synthetic.txt').read_bytes(), b'original source')
        self.assertEqual((output / 'python/python313._pth').read_text(), 'python313.zip\n.\n../tools\n')
        self.assertTrue(manifest['source_only_distribution'])
        self.assertFalse(manifest['contains_precompiled_game'])
        self.assertTrue(manifest['requires_local_game_compilation'])
        for row in manifest['files']:
            file = output / row['path']
            self.assertEqual(file.stat().st_size, row['bytes'])
            self.assertEqual(hashlib.sha256(file.read_bytes()).hexdigest(), row['sha256'])
        self.assertEqual(json.loads((output / 'package-manifest.json').read_text()), manifest)
        self.assertIn('No precompiled game executable', ' '.join((output / 'README.txt').read_text().split()))
        self.assertEqual(list(self.output_root.glob('.badman-player-*')), [])

    def notice_fixture(self):
        directory = self.root / 'dependency config'; directory.mkdir()
        config = json.loads((package.PROFILE / 'config/toolchain_dependencies.json').read_text(encoding='utf-8'))
        codec = json.loads((package.PROFILE / 'config/video_codec.json').read_text(encoding='utf-8'))
        def write():
            (directory / 'toolchain_dependencies.json').write_text(json.dumps(config), encoding='utf-8')
            (directory / 'video_codec.json').write_text(json.dumps(codec), encoding='utf-8')
        write()
        entries = [package.Entry(directory / name, 'config/' + name, 'dependency-config')
                   for name in ('toolchain_dependencies.json', 'video_codec.json')]
        return config, codec, write, entries

    def test_shipped_notices_match_current_downloaded_dependency(self):
        config, codec, _, entries = self.notice_fixture()
        self.assemble('current notices', entries=entries)
        notice = (self.output_root / 'current notices/THIRD_PARTY_NOTICES.txt').read_text(encoding='utf-8')
        for value in (codec['version'], config['ffmpeg']['release_tag'], config['ffmpeg']['source_commit'],
                      config['ffmpeg']['source_url'], config['ffmpeg']['sha256']):
            self.assertIn(value, notice)
        self.assertNotIn(package.FFMPEG_IDENTITY['source_commit'], notice)

    def test_dependency_rotation_uses_staged_config_not_packager_legacy_facts(self):
        config, codec, write, entries = self.notice_fixture()
        config['ffmpeg'].update(source_commit='a' * 40, source_url='https://github.com/FFmpeg/FFmpeg/tree/' + 'a' * 40,
                                release_tag='synthetic-next-release', sha256='b' * 64)
        codec.update(source_commit=config['ffmpeg']['source_commit'], source_url=config['ffmpeg']['source_url'],
                     version='synthetic-next-version', archive_sha256=config['ffmpeg']['sha256'])
        write()
        self.assemble('rotated notices', entries=entries)
        notice = (self.output_root / 'rotated notices/THIRD_PARTY_NOTICES.txt').read_text(encoding='utf-8')
        for value in ('synthetic-next-version', 'synthetic-next-release', 'a' * 40, 'b' * 64):
            self.assertIn(value, notice)
        current = json.loads((package.PROFILE / 'config/toolchain_dependencies.json').read_text(encoding='utf-8'))
        self.assertNotIn(current['ffmpeg']['source_commit'], notice)

    def test_conflicting_or_missing_notice_identity_cannot_publish(self):
        config, codec, write, entries = self.notice_fixture()
        original = dict(codec)
        for key, value in [('source_commit', 'c' * 40), ('archive_sha256', 'd' * 64),
                           ('archive_size', codec['archive_size'] + 1), ('version', 'bad\nnotice')]:
            codec.clear(); codec.update(original); codec[key] = value; write()
            with self.subTest(key=key):
                with self.assertRaises(package.PackageError): self.assemble(entries=entries)
                self.assertEqual(list(self.output_root.iterdir()), [])
        codec.clear(); codec.update(original); write()
        with self.assertRaises(package.PackageError): self.assemble(entries=entries[:1])
        self.assertEqual(list(self.output_root.iterdir()), [])

    def test_existing_unowned_and_owned_outputs_preserved(self):
        output = self.output_root / 'player'; output.mkdir()
        marker = output / 'user.txt'; marker.write_text('keep me')
        with self.assertRaises(package.PackageError): self.assemble()
        self.assertEqual(marker.read_text(), 'keep me')
        self.assemble('owned')
        before = package.digest(self.output_root / 'owned/package-manifest.json')
        with self.assertRaises(package.PackageError): self.assemble('owned')
        self.assertEqual(package.digest(self.output_root / 'owned/package-manifest.json'), before)

    def test_bad_runtime_hash_never_publishes_or_deletes_sources(self):
        with self.assertRaises(package.PackageError): self.assemble(expected_hash='0' * 64)
        self.assertFalse((self.output_root / 'player').exists())
        self.assertEqual(self.source.read_bytes(), b'original source')
        self.assertEqual(list(self.output_root.iterdir()), [])

    def test_zip_traversal_and_windows_aliases_rejected(self):
        for name in ('../escape.txt', '/escape.txt', 'C:/escape.txt', '..\\escape.txt', 'NUL.txt'):
            with self.subTest(name=name):
                self.make_archive([(name, b'bad')])
                with self.assertRaises(package.PackageError): self.assemble()
                self.assertEqual(list(self.output_root.iterdir()), [])
        self.assertFalse((self.root / 'escape.txt').exists())

    def test_zip_duplicate_case_collision_and_symlink_rejected(self):
        self.make_archive([('PYTHON.EXE', b'collision')])
        with self.assertRaises(package.PackageError): self.assemble()
        self.make_archive()
        with zipfile.ZipFile(self.archive, 'a') as archive:
            link = zipfile.ZipInfo('linked'); link.create_system = 3; link.external_attr = 0o120777 << 16
            archive.writestr(link, b'../elsewhere')
        self.archive_hash = package.digest(self.archive)
        with self.assertRaises(package.PackageError): self.assemble()

    def test_no_game_inputs_or_metadata_collisions(self):
        for path in ('game.iso', 'PSP_GAME/EBOOT.ELF', 'font.prx', 'snapshot.bin', 'README.txt', 'python/python.exe', '../outside.txt',
                     'BadmanNative.exe', 'runtime-source/BadmanNative.exe', 'badman_font_aot.lib', 'libbadman_aot.a', 'badman_aot.dll', 'host.obj',
                     'runtime-source/generated/foo.cpp', 'runtime-source/profiles/badman/game/foo.cpp',
                     'runtime-source/src/badman_unit_0000.cpp', 'runtime-source/src/generated_registry.cpp', 'runtime-source/profiles/badman/host/badman_font_unit_0000.cpp',
                     'runtime-source/profiles/badman/host/badman_font_imports.cpp', 'runtime-source/profiles/badman/host/badman_font_registry.cpp', 'unknown.exe',
                     'tools/ffmpeg/bin/avcodec-62.dll', 'tools/ffmpeg/include/libavcodec/avcodec.h', 'tools/pspdecrypt/pspdecrypt-utf8.exe', 'tools/7zip/7z.exe'):
            with self.subTest(path=path):
                with self.assertRaises(package.PackageError):
                    self.assemble(entries=[package.Entry(self.source, path, 'synthetic')])
        with self.assertRaises(package.PackageError):
            self.assemble(entries=[package.Entry(self.source, 'A.txt', 'x'), package.Entry(self.source, 'a.txt', 'x')])

    def test_output_cannot_escape_its_owned_root(self):
        with self.assertRaises(package.PackageError):
            package.assemble(self.root / 'outside', [], self.archive, self.output_root, self.archive_hash)
        self.assertFalse((self.root / 'outside').exists())

    def test_missing_runtime_members_and_sources_preserve_output(self):
        with zipfile.ZipFile(self.archive, 'w') as archive: archive.writestr('python.exe', b'not enough')
        self.archive_hash = package.digest(self.archive)
        with self.assertRaises(package.PackageError): self.assemble()
        self.assertEqual(list(self.output_root.iterdir()), [])
        with self.assertRaises(package.PackageError):
            self.assemble(entries=[package.Entry(self.root / 'missing', 'missing.txt', 'x')])

    def test_pe_preflight_rejects_wrong_builds(self):
        binary = self.root / 'native.exe'; data = bytearray(320)
        data[:2] = b'MZ'; struct.pack_into('<I', data, 0x3c, 64); data[64:68] = b'PE\0\0'
        struct.pack_into('<H', data, 68, 0x8664); struct.pack_into('<H', data, 88, 0x20b)
        struct.pack_into('<H', data, 156, 3); data[200:224] = b'activeCodePage UTF-8 test'
        binary.write_bytes(data); package.verify_pe(binary, 3)
        with self.assertRaises(package.PackageError): package.verify_pe(binary, 2)
        data[200:224] = b'no manifest here........'; binary.write_bytes(data)
        with self.assertRaises(package.PackageError): package.verify_pe(binary, 3)

    def codec_fixture(self):
        vendor = self.root / '코덱 runtime'; (vendor / 'bin').mkdir(parents=True)
        provenance = self.root / '코덱 provenance'; provenance.mkdir()
        archive = self.root / 'ffmpeg.zip'; archive.write_bytes(b'explicit synthetic archive identity')
        identity = dict(package.FFMPEG_IDENTITY, archive_sha256=package.digest(archive), archive_size=archive.stat().st_size)
        vendor_pins = {}
        for name in package.FFMPEG_VENDOR_FILES:
            target = vendor / name
            if name.endswith('.dll'):
                body = bytearray(256); body[:2] = b'MZ'; struct.pack_into('<I', body, 0x3c, 64)
                body[64:68] = b'PE\0\0'; struct.pack_into('<H', body, 68, 0x8664)
                struct.pack_into('<H', body, 86, 0x2000); struct.pack_into('<H', body, 88, 0x20b)
                body.extend(name.encode())
            else: body = b'synthetic LGPL fixture, not a vendor license'
            target.write_bytes(body); vendor_pins[name] = (len(body), package.digest(target))
        # Distractors prove that no prefix/SDK recursive copy is used.
        (vendor / 'bin/ffmpeg.exe').write_bytes(b'do not package this executable')
        (vendor / 'avcodec.h').write_bytes(b'do not package this header')
        record = dict(identity, files=[{'path': name, 'bytes': size, 'sha256': checksum}
            for name, (size, checksum) in vendor_pins.items()])
        provenance_pins = {}
        for name in package.FFMPEG_PROVENANCE_FILES:
            target = provenance / name
            if name == 'provenance.json': target.write_text(json.dumps(record), encoding='utf-8')
            else: target.write_text('synthetic source/build/license record: ' + name, encoding='utf-8')
            provenance_pins[name] = (target.stat().st_size, package.digest(target))
        config = self.root / 'video_codec.json'
        config.write_text(json.dumps(dict(identity, required_runtime_dlls=[Path(name).name
            for name in vendor_pins if name.endswith('.dll')])), encoding='utf-8')
        return {'directory': vendor, 'archive': archive, 'provenance_directory': provenance,
            'codec_config': config, 'identity': identity, 'vendor_pins': vendor_pins, 'provenance_pins': provenance_pins}

    def test_ffmpeg_exact_dll_license_provenance_allowlist(self):
        fixture = self.codec_fixture(); entries = package.ffmpeg_entries(**fixture)
        expected = {('tools/ffmpeg/' + name) for name in package.FFMPEG_VENDOR_FILES}
        expected |= {('tools/ffmpeg/' + name) for name in package.FFMPEG_PROVENANCE_FILES}
        expected.add('config/video_codec.json')
        self.assertEqual({entry.relative for entry in entries}, expected)
        self.assertEqual(sum(entry.relative.endswith('.dll') for entry in entries), 4)
        self.assertFalse(any(entry.relative.endswith(('.h', '.exe')) for entry in entries))
        # Verification helpers remain useful for local developer checks, but
        # vendor codec binaries are no longer distribution payloads.
        with self.assertRaises(package.PackageError): self.assemble('codec 한글 player', entries=entries)
        self.assertEqual(list(self.output_root.glob('.badman-player-*')), [])

    def test_ffmpeg_missing_runtime_dependency_or_licenses_refused(self):
        fixture = self.codec_fixture()
        for folder, pins in ((fixture['directory'], fixture['vendor_pins']),
                             (fixture['provenance_directory'], fixture['provenance_pins'])):
            for name in pins:
                with self.subTest(name=name):
                    source = folder / name; saved = source.read_bytes(); source.unlink()
                    try:
                        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)
                    finally: source.write_bytes(saved)
        self.assertEqual(list(self.output_root.iterdir()), [])

    def test_ffmpeg_tampered_archive_and_dll_refused(self):
        fixture = self.codec_fixture(); archive = fixture['archive']; saved = archive.read_bytes()
        archive.write_bytes(b'X' + saved[1:])
        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)
        archive.write_bytes(saved)
        source = fixture['directory'] / 'bin/avcodec-62.dll'; saved = source.read_bytes()
        source.write_bytes(saved[:-1] + b'X')
        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)
        self.assertEqual(list(self.output_root.iterdir()), [])

    def test_ffmpeg_change_after_preflight_cannot_publish(self):
        fixture = self.codec_fixture(); entries = package.ffmpeg_entries(**fixture)
        source = fixture['directory'] / 'bin/swresample-6.dll'; saved = source.read_bytes()
        source.write_bytes(saved[:-1] + b'X')
        with self.assertRaises(package.PackageError): self.assemble(entries=entries)
        self.assertEqual(list(self.output_root.iterdir()), [])
        self.assertEqual(source.read_bytes(), saved[:-1] + b'X')

    def test_ffmpeg_forged_provenance_does_not_override_pin(self):
        fixture = self.codec_fixture(); path = fixture['provenance_directory'] / 'provenance.json'
        record = json.loads(path.read_text()); record['source_commit'] = '0' * 40
        path.write_text(json.dumps(record), encoding='utf-8')
        fixture['provenance_pins']['provenance.json'] = (path.stat().st_size, package.digest(path))
        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)

    def test_ffmpeg_config_must_declare_transitive_runtime_dependency(self):
        fixture = self.codec_fixture(); path = fixture['codec_config']; record = json.loads(path.read_text())
        record['required_runtime_dlls'].remove('swresample-6.dll'); path.write_text(json.dumps(record))
        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)

    def test_ffmpeg_unsafe_member_paths_and_linked_directory_refused(self):
        fixture = self.codec_fixture(); fixture['vendor_pins']['../escape.dll'] = (1, '0' * 64)
        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)
        fixture['vendor_pins'].pop('../escape.dll')
        original = package.linked
        with mock.patch.object(package, 'linked', side_effect=lambda path:
                Path(path) == fixture['directory'] / 'bin' or original(path)):
            with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)
        self.assertFalse((self.root / 'escape.dll').exists())

    def test_ffmpeg_wrong_architecture_or_executable_is_not_a_dll(self):
        fixture = self.codec_fixture(); source = fixture['directory'] / 'bin/avcodec-62.dll'
        saved = source.read_bytes()
        for offset, value in ((68, 0x14c), (86, 0), (88, 0x10b)):
            body = bytearray(saved); struct.pack_into('<H', body, offset, value); source.write_bytes(body)
            fixture['vendor_pins']['bin/avcodec-62.dll'] = (len(body), package.digest(source))
            with self.subTest(offset=offset):
                with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)

    def source_fixture(self):
        root = self.root / 'independent source'; root.mkdir()
        names = ['CMakeLists.txt', 'src/runtime.cpp', 'tools/codegen_main.cpp', 'profiles/badman/CMakeLists.txt',
                 'profiles/badman/tools/generate.py', 'profiles/badman/tools/generate_font.py', 'profiles/badman/host/main.cpp']
        files = []
        for name in names:
            path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('synthetic independent source')
            files.append({'path': name, 'bytes': path.stat().st_size, 'sha256': package.digest(path)})
        manifest = self.root / 'reviewed-sources.json'
        manifest.write_text(json.dumps({'schema': 1, 'game_independent_source_only': True, 'files': files}))
        return root, manifest

    def test_exact_source_inventory_and_distractors(self):
        root, manifest = self.source_fixture()
        (root / 'BadmanNative.exe').write_bytes(b'do not include')
        (root / 'src/unreviewed.cpp').write_text('do not include')
        entries = package.source_code_entries(root, manifest)
        self.assertEqual(len(entries), 8)
        names = {entry.relative for entry in entries}
        self.assertNotIn('runtime-source/BadmanNative.exe', names)
        self.assertNotIn('runtime-source/src/unreviewed.cpp', names)
        self.assemble(entries=entries)

    def test_source_inventory_rejects_game_paths_duplicates_tamper_and_missing(self):
        root, manifest = self.source_fixture(); record = json.loads(manifest.read_text())
        for bad in ('profiles/badman/game/translated.cpp', 'profiles/badman/analysis/snapshot.cpp',
                    'profiles/badman/generated/badman_unit_0000.cpp', 'src/badman_unit_0000.cpp', 'profiles/badman/host/badman_font_unit_0000.cpp', 'BadmanNative.exe', '../escape.cpp'):
            altered = dict(record, files=record['files'] + [dict(record['files'][0], path=bad)])
            manifest.write_text(json.dumps(altered))
            with self.subTest(path=bad):
                with self.assertRaises(package.PackageError): package.source_code_entries(root, manifest)
        manifest.write_text(json.dumps(dict(record, files=record['files'] + [record['files'][0]])))
        with self.assertRaises(package.PackageError): package.source_code_entries(root, manifest)
        manifest.write_text(json.dumps(record)); source = root / 'src/runtime.cpp'; source.write_text('tampered')
        with self.assertRaises(package.PackageError): package.source_code_entries(root, manifest)
        source.unlink()
        with self.assertRaises(package.PackageError): package.source_code_entries(root, manifest)

    def vulkan_source_fixture(self):
        root, manifest = self.source_fixture()
        record = json.loads(manifest.read_text())
        for name in package.VULKAN_HEADERS_FILES:
            relative = package.VULKAN_HEADERS_ROOT + name
            source = package.ROOT / relative; target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source, target)
            record['files'].append({'path': relative, 'bytes': target.stat().st_size, 'sha256': package.digest(target)})
        manifest.write_text(json.dumps(record))
        return root, manifest, record

    def test_vulkan_header_closure_and_licenses_ship_with_exact_notice(self):
        root, manifest, _ = self.vulkan_source_fixture()
        entries = package.source_code_entries(root, manifest)
        self.assemble('vulkan-source', entries=entries)
        output = self.output_root / 'vulkan-source'
        for name in package.VULKAN_HEADERS_FILES:
            self.assertEqual(package.digest(output / ('runtime-source/' + package.VULKAN_HEADERS_ROOT + name)),
                             package.digest(root / (package.VULKAN_HEADERS_ROOT + name)))
        notice = (output / 'THIRD_PARTY_NOTICES.txt').read_text(encoding='utf-8')
        self.assertIn(package.VULKAN_HEADERS_COMMIT, notice)
        self.assertIn('SPDX Apache-2.0', notice); self.assertIn('MIT', notice)
        self.assertIn('No Vulkan driver, SDK or DXC shader compiler is bundled', notice)

    def test_arbitrary_third_party_and_nonhost_shader_paths_are_rejected(self):
        root, manifest = self.source_fixture(); original = json.loads(manifest.read_text())
        for name in ('third_party/unreviewed/code.hpp', 'third_party/vulkan_headers/include/vulkan/extra.h',
                     'third_party/vulkan_headers/other.json', 'src/unreviewed.hlsl'):
            file = root / name; file.parent.mkdir(parents=True, exist_ok=True); file.write_text('synthetic unwanted source')
            record = dict(original, files=original['files'] + [{'path': name, 'bytes': file.stat().st_size, 'sha256': package.digest(file)}])
            manifest.write_text(json.dumps(record))
            with self.subTest(path=name), self.assertRaises(package.PackageError):
                package.source_code_entries(root, manifest)
        for name in ('runtime-source/third_party/unreviewed/code.hpp', 'runtime-source/third_party/vulkan_headers/include/vulkan/extra.h'):
            with self.subTest(assembly_path=name), self.assertRaises(package.PackageError):
                self.assemble('unwanted', entries=[package.Entry(self.source, name, 'synthetic')])

    def test_vulkan_missing_license_and_repin_tamper_cannot_publish(self):
        root, manifest, original = self.vulkan_source_fixture()
        for suffix in ('LICENSES/Apache-2.0.txt', 'LICENSES/MIT.txt', 'PROVENANCE.json'):
            name = package.VULKAN_HEADERS_ROOT + suffix
            manifest.write_text(json.dumps(dict(original, files=[r for r in original['files'] if r['path'] != name])))
            with self.subTest(missing=suffix), self.assertRaises(package.PackageError):
                package.source_code_entries(root, manifest)
        manifest.write_text(json.dumps(original))
        header = root / (package.VULKAN_HEADERS_ROOT + 'include/vulkan/vulkan_core.h')
        header.write_bytes(header.read_bytes() + b'\n// tampered after vendor provenance\n')
        for row in original['files']:
            if row['path'].endswith('/vulkan_core.h'):
                row.update(bytes=header.stat().st_size, sha256=package.digest(header))
        manifest.write_text(json.dumps(original))
        with self.assertRaisesRegex(package.PackageError, 'Pinned Vulkan-Headers input mismatch'):
            package.source_code_entries(root, manifest)

    def test_independent_host_hlsl_is_an_explicit_reviewed_source(self):
        root, manifest = self.source_fixture(); record = json.loads(manifest.read_text())
        name = 'profiles/badman/host/independent_shader.hlsl'; file = root / name
        file.write_text('// independently written shader fixture')
        record['files'].append({'path': name, 'bytes': file.stat().st_size, 'sha256': package.digest(file)})
        manifest.write_text(json.dumps(record))
        self.assertIn('runtime-source/' + name, {e.relative for e in package.source_code_entries(root, manifest)})

    def test_matching_sdk_pins_required_for_local_compile(self):
        fixture = self.codec_fixture(); vendor = fixture['directory']; files = []
        for name in ('libavcodec/avcodec.h', 'libavutil/avutil.h', 'libswscale/swscale.h', 'libswresample/swresample.h'):
            source = vendor / 'include' / name; source.parent.mkdir(parents=True, exist_ok=True); source.write_text('synthetic public header')
            files.append(dict(path='include/' + name, bytes=source.stat().st_size, sha256=package.digest(source)))
        manifest = self.root / 'sdk.json'
        record = dict(archive_sha256=fixture['identity']['archive_sha256'], source_commit=fixture['identity']['source_commit'], files=files)
        manifest.write_text(json.dumps(record)); fixture['sdk_manifest'] = manifest
        entries = package.ffmpeg_entries(**fixture)
        self.assertEqual(sum(entry.relative.endswith('.h') for entry in entries), 4)
        self.assertFalse(any(entry.relative.endswith('.exe') for entry in entries))
        header = vendor / files[0]['path']; header.write_text('wrong version')
        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)
        record['files'] = record['files'][1:]; manifest.write_text(json.dumps(record))
        with self.assertRaises(package.PackageError): package.ffmpeg_entries(**fixture)


if __name__ == '__main__':
    unittest.main()
