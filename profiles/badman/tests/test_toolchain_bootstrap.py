"""Dependency checks without network/game execution; owned Python crash probes."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import zipfile

PATH = Path(__file__).resolve().parents[1] / 'tools/toolchain_bootstrap.py'
SPEC = importlib.util.spec_from_file_location('bootstrap_subject', PATH)
module = importlib.util.module_from_spec(SPEC); sys.modules[SPEC.name] = module; SPEC.loader.exec_module(module)


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='badman-deps-test-')
        self.root = Path(self.temporary.name)
        self.children = []
    def tearDown(self):
        for child in self.children:
            if child.poll() is None: child.kill(); child.wait(timeout=10)
        self.temporary.cleanup()
    def record(self, data=b'synthetic archive'):
        return {'url': 'https://publisher.invalid/archive.zip', 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    def test_owner_written_before_work_and_unrelated_content_preserved(self):
        data = self.root / 'data'; cache = module.own_cache(data, False)
        self.assertTrue(module.owns_dependency_directory(data)); self.assertTrue((cache / module.OWNER).is_file())
        (cache / 'foreign.txt').write_text('preserve')
        self.assertFalse(module.owns_dependency_directory(data))
        with self.assertRaises(module.DependencyError): module.own_cache(data, False)
        self.assertEqual((cache / 'foreign.txt').read_text(), 'preserve')
    def test_verify_only_never_creates_missing_root(self):
        data = self.root / 'missing'
        with self.assertRaises(module.DependencyError): module.own_cache(data, True)
        self.assertFalse(data.exists())
        output = self.root / 'download'
        with self.assertRaises(module.DependencyError): module.obtain_file(output, self.record(), lambda _: None, verify_only=True)
        self.assertFalse(output.exists())
    def test_unowned_directory_or_bad_marker_is_preserved(self):
        for index, marker in enumerate([None, {'producer': 'another app', 'schema': 1}, {'producer': module.PRODUCER, 'schema': 99}]):
            data = self.root / str(index); cache = data / 'tools'; cache.mkdir(parents=True)
            if marker is not None: (cache / module.OWNER).write_text(json.dumps(marker))
            with self.assertRaises(module.DependencyError): module.own_cache(data, False)
            self.assertTrue(cache.exists())
    def test_seed_and_reuse_are_verified_without_network(self):
        seed = self.root / 'seed.zip'; seed.write_bytes(b'synthetic archive'); target = self.root / 'cache/archive.zip'
        with mock.patch.object(module.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            module.obtain_file(target, self.record(), lambda _: None, seed)
            self.assertEqual(module.obtain_file(target, self.record(), lambda _: None, verify_only=True), target)
        target.write_bytes(b'tampered')
        with self.assertRaises(module.DependencyError): module.obtain_file(target, self.record(), lambda _: None, seed)
        self.assertEqual(target.read_bytes(), b'tampered')
    def test_bad_seed_cannot_publish_and_original_is_preserved(self):
        seed = self.root / 'seed'; seed.write_bytes(b'wrong'); target = self.root / 'target'
        with self.assertRaises(module.DependencyError): module.obtain_file(target, self.record(), lambda _: None, seed)
        self.assertFalse(target.exists()); self.assertEqual(seed.read_bytes(), b'wrong')
    def test_https_download_is_streamed_and_oversize_or_hash_failure_never_publish(self):
        target = self.root / 'target'; record = self.record()
        for body in (b'wrong archive', b'X' * (record['bytes'] + 1)):
            with mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(body)):
                with self.assertRaises(module.DependencyError): module.obtain_file(target, record, lambda _: None)
            self.assertFalse(target.exists()); self.assertEqual(list(self.root.glob('*.part-*')), [])
        with mock.patch.object(module.urllib.request, 'urlopen', return_value=io.BytesIO(b'synthetic archive')):
            module.obtain_file(target, record, lambda _: None)
        self.assertEqual(target.read_bytes(), b'synthetic archive')
    def test_archive_traversal_aliases_duplicates_links_and_bounded_size(self):
        for name in ('../escape', '/escape', 'C:/escape', '..\\escape', 'NUL.txt'):
            archive = self.root / 'bad.zip'
            with zipfile.ZipFile(archive, 'w') as handle: handle.writestr(name, b'x')
            with self.assertRaises(module.DependencyError): module.extract_zip(archive, self.root / 'stage')
        with zipfile.ZipFile(archive, 'w') as handle:
            handle.writestr('a.txt', b'x'); handle.writestr('A.txt', b'x')
        with self.assertRaises(module.DependencyError): module.extract_zip(archive, self.root / 'case')
        with zipfile.ZipFile(archive, 'w') as handle:
            row = zipfile.ZipInfo('link'); row.create_system = 3; row.external_attr = 0o120777 << 16; handle.writestr(row, '../outside')
        with self.assertRaises(module.DependencyError): module.extract_zip(archive, self.root / 'link')
        with zipfile.ZipFile(archive, 'w') as handle: handle.writestr('large', b'xx')
        with self.assertRaises(module.DependencyError): module.extract_zip(archive, self.root / 'size', max_bytes=1)
        self.assertFalse((self.root.parent / 'escape').exists())
    def test_concurrent_or_verify_only_cache_lock(self):
        folder = module.own_cache(self.root / 'data', False)
        with module.cache_lock(folder, False):
            with self.assertRaises(module.DependencyError):
                with module.cache_lock(folder, False): pass
            with self.assertRaises(module.DependencyError):
                with module.cache_lock(folder, True): pass
        self.assertFalse((folder / '.lock').exists())

    def start_holder(self, folder, mode='lock', seed=None):
        ready, log = self.root / ('ready-' + mode), self.root / ('child-' + mode + '.log')
        code = r'''
import importlib.util,json,pathlib,sys,tempfile,time
p=pathlib.Path(sys.argv[1]);s=importlib.util.spec_from_file_location('owned_bootstrap_probe',p)
m=importlib.util.module_from_spec(s);exec(compile(p.read_bytes(),str(p),'exec'),m.__dict__)
folder=pathlib.Path(sys.argv[2]);ready=pathlib.Path(sys.argv[3]);mode=sys.argv[4]
with m.cache_lock(folder,False):
    if mode=='publish':
        seed=pathlib.Path(sys.argv[5])
        def blocked_publish(temporary,destination):
            ready.write_text(str(temporary),encoding='utf-8')
            while True:time.sleep(.05)
        m.publish_file=blocked_publish
        m.obtain_file(folder/'downloads/archive.zip',{'url':'https://invalid.example/unused','bytes':seed.stat().st_size,'sha256':m.sha(seed)},lambda _:None,seed)
    elif mode=='extract':
        with tempfile.TemporaryDirectory(prefix='.ffmpeg-',dir=folder) as stage:
            m.extract_zip(pathlib.Path(sys.argv[5]),pathlib.Path(stage))
            ready.write_text(stage,encoding='utf-8')
            while True:time.sleep(.05)
    else:
        ready.write_text('locked',encoding='utf-8')
        while True:time.sleep(.05)
'''
        with log.open('wb') as output:
            child = subprocess.Popen([sys.executable, '-u', '-X', 'utf8', '-c', code,
                str(PATH), str(folder), str(ready), mode, str(seed or '')], stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.children.append(child)
        deadline=time.monotonic()+10
        while not ready.is_file() and time.monotonic()<deadline:
            if child.poll() is not None: self.fail('Owned lock probe failed: '+log.read_text(encoding='utf-8',errors='replace'))
            time.sleep(.02)
        self.assertTrue(ready.is_file(), 'Owned probe did not rendezvous')
        return child, ready

    @staticmethod
    def precise_snapshot(folder):
        return {p.relative_to(folder).as_posix(): (p.stat().st_size,p.stat().st_mtime_ns,module.sha(p))
                for p in folder.rglob('*') if p.is_file()}

    def test_terminated_owner_releases_lock_without_changing_legacy_or_readonly_files(self):
        data=self.root/'data';folder=module.own_cache(data,False)
        # A live/reused PID is not a kernel lock and must not need deletion.
        legacy=folder/'.lock';legacy.write_text(str(os.getpid()),encoding='ascii')
        before=self.precise_snapshot(data)
        child,_=self.start_holder(folder)
        self.assertTrue(module.owns_dependency_directory(data), 'Coordination must not block reading OWNER')
        self.assertEqual(module.own_cache(data,False),folder)
        for readonly in (False,True):
            with self.assertRaises(module.DependencyError) as caught:
                with module.cache_lock(folder,readonly):pass
            self.assertEqual(caught.exception.reason,'dependency_cache_busy')
        child.terminate();child.wait(timeout=10)
        for readonly in (False,True):
            with module.cache_lock(folder,readonly):pass
        self.assertEqual(self.precise_snapshot(data),before)

    def test_verify_only_lock_without_legacy_file_never_creates_or_changes_metadata(self):
        data=self.root/'data';folder=module.own_cache(data,False);before=self.precise_snapshot(data)
        with module.cache_lock(folder,True):pass
        self.assertEqual(self.precise_snapshot(data),before)
        self.assertFalse((folder/'.lock').exists())

    def test_oversized_legacy_lock_is_preserved_and_not_adopted(self):
        data=self.root/'data';folder=module.own_cache(data,False)
        (folder/'.lock').write_bytes(b'x'*4097);before=self.precise_snapshot(data)
        self.assertFalse(module.owns_dependency_directory(data))
        with self.assertRaises(module.DependencyError):module.own_cache(data,False)
        with self.assertRaises(module.DependencyError):
            with module.cache_lock(folder,True):pass
        self.assertEqual(self.precise_snapshot(data),before)

    def test_whole_dependency_path_reports_busy_while_other_process_holds_cache(self):
        fixture=self.migration_fixture();child,_=self.start_holder(fixture['tools'])
        before=self.precise_snapshot(fixture['data'])
        for readonly in (False,True):
            with self.assertRaises(module.DependencyError) as caught:self.migrate(fixture,verify_only=readonly)
            self.assertEqual(caught.exception.reason,'dependency_cache_busy')
        self.assertEqual(self.precise_snapshot(fixture['data']),before)
        child.terminate();child.wait(timeout=10)
        self.migrate(fixture)

    def test_terminated_publish_keeps_final_absent_then_retry_and_readonly_succeed(self):
        data=self.root/'data';folder=module.own_cache(data,False)
        seed=self.root/'seed.zip';seed.write_bytes(b'verified archive bytes'*4096)
        record=self.record(seed.read_bytes());target=folder/'downloads/archive.zip'
        child,ready=self.start_holder(folder,'publish',seed)
        part=Path(ready.read_text(encoding='utf-8'))
        self.assertFalse(target.exists());self.assertEqual(part.read_bytes(),seed.read_bytes())
        child.terminate();child.wait(timeout=10)
        part_before=(part.stat().st_size,part.stat().st_mtime_ns,module.sha(part))
        with module.cache_lock(folder,False):module.obtain_file(target,record,lambda _:None,seed)
        self.assertEqual(target.read_bytes(),seed.read_bytes())
        self.assertEqual((part.stat().st_size,part.stat().st_mtime_ns,module.sha(part)),part_before)
        before=self.precise_snapshot(data)
        with module.cache_lock(folder,True):module.obtain_file(target,record,lambda _:None,verify_only=True)
        self.assertEqual(self.precise_snapshot(data),before)

    def test_atomic_publish_preserves_racing_foreign_final(self):
        seed=self.root/'seed';seed.write_bytes(b'known archive');target=self.root/'archive.zip'
        original=module.publish_file
        def race(temporary,destination):
            Path(destination).write_bytes(b'foreign final created during preflight')
            return original(temporary,destination)
        with mock.patch.object(module,'publish_file',side_effect=race):
            with self.assertRaises(module.DependencyError):module.obtain_file(target,self.record(seed.read_bytes()),lambda _:None,seed)
        self.assertEqual(target.read_bytes(),b'foreign final created during preflight')
        self.assertEqual(list(self.root.glob('*.part-*')),[])

    def test_terminated_extraction_stage_is_preserved_but_never_adopted(self):
        fixture=self.migration_fixture();child,ready=self.start_holder(fixture['tools'],'extract',fixture['seed'])
        stage=Path(ready.read_text(encoding='utf-8'))
        child.terminate();child.wait(timeout=10)
        before=self.precise_snapshot(stage)
        self.assertTrue(module.owns_dependency_directory(fixture['data']))
        result=self.migrate(fixture)
        self.assertTrue(result['ffmpeg_prefix'].is_dir())
        self.assertNotEqual(result['ffmpeg_prefix'].parent,stage)
        self.assertEqual(self.precise_snapshot(stage),before)
        full=self.precise_snapshot(fixture['data'])
        self.migrate(fixture,verify_only=True)
        self.assertEqual(self.precise_snapshot(fixture['data']),full)

    def test_interrupted_names_bounded_trees_and_foreign_content_preserved(self):
        data=self.root/'data';folder=module.own_cache(data,False)
        for name in ('.ffmpeg-abcdefgh','.pspdecrypt-hijklmno'):
            stage=folder/name;stage.mkdir();(stage/'unfinished.txt').write_bytes(b'partial, not adopted')
            before=self.precise_snapshot(stage)
            self.assertTrue(module.owns_dependency_directory(data))
            self.assertEqual(module.own_cache(data,False),folder)
            self.assertEqual(self.precise_snapshot(stage),before)
        unknown=folder/'.ffmpeg-user-notes';unknown.mkdir();(unknown/'keep').write_text('foreign')
        before=self.precise_snapshot(data)
        with self.assertRaises(module.DependencyError):module.own_cache(data,False)
        self.assertEqual(self.precise_snapshot(data),before)

    def test_interrupted_stage_links_and_size_bounds_do_not_grant_ownership(self):
        data=self.root/'data';folder=module.own_cache(data,False);stage=folder/'.ffmpeg-abcdefgh';stage.mkdir()
        payload=stage/'payload';payload.write_bytes(b'longer than test bound')
        with mock.patch.object(module,'STAGE_MAX_BYTES',1):self.assertFalse(module.owns_dependency_directory(data))
        alias=self.root/'foreign';os.link(payload,alias)
        self.assertFalse(module.owns_dependency_directory(data))
        self.assertEqual(alias.read_bytes(),payload.read_bytes())
    def codec_fixture(self):
        prefix = self.root / 'FFmpeg'; sdk = {'files': []}; runtime = {}
        for name in ['bin/avcodec-62.dll', 'LICENSE.txt', 'include/libavcodec/avcodec.h']:
            path = prefix / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(name.encode())
            record = {'bytes': path.stat().st_size, 'sha256': module.sha(path)}
            if name.startswith('include/'): sdk['files'].append(dict(record, path=name))
            else: runtime[name] = record
        return prefix, {'ffmpeg_runtime_files': runtime}, sdk
    def test_matching_dll_and_all_headers_are_required(self):
        prefix, config, sdk = self.codec_fixture(); module.verify_ffmpeg(prefix, config, sdk)
        source = prefix / 'include/libavcodec/avcodec.h'; source.write_text('different version')
        with self.assertRaises(module.DependencyError): module.verify_ffmpeg(prefix, config, sdk)
        source.unlink()
        with self.assertRaises(module.DependencyError): module.verify_ffmpeg(prefix, config, sdk)
    def test_verified_explicit_tools_do_not_create_data_root(self):
        prefix, config, sdk = self.codec_fixture(); exe = self.root / 'verified-decryptor'; exe.write_bytes(b'known working copy')
        config['pspdecrypt'] = {'known_utf8_sha256': module.sha(exe)}
        with mock.patch.object(module, 'load_configuration', return_value=(config, sdk)), mock.patch.object(module, 'installed_sevenzip', return_value=self.root / '7z.exe'):
            data = self.root / 'absent'; result = module.ensure_dependencies(data, decryptor=exe, ffmpeg_prefix=prefix)
            self.assertEqual(result['ffmpeg_prefix'], prefix); self.assertFalse(data.exists())
            exe.write_bytes(b'wrong copy')
            with self.assertRaises(module.DependencyError): module.ensure_dependencies(data, decryptor=exe, ffmpeg_prefix=prefix)
    def test_nonresource_pe_section_fingerprint_detects_code_changes(self):
        data = bytearray(1024); data[:2] = b'MZ'; struct.pack_into('<I', data, 60, 64); data[64:68] = b'PE\0\0'
        struct.pack_into('<H', data, 70, 2); struct.pack_into('<H', data, 84, 0)
        for pos, name, start in ((88, b'.text', 512), (128, b'.rsrc', 768)):
            data[pos:pos+len(name)] = name; struct.pack_into('<II', data, pos+16, 16, start)
        path = self.root / 'fake.exe'; path.write_bytes(data); original = module.pe_sections(path)
        data[768] = 1; path.write_bytes(data); changed = module.pe_sections(path)
        self.assertEqual(original[b'.text'], changed[b'.text']); self.assertNotEqual(original[b'.rsrc'], changed[b'.rsrc'])
        data[512] = 1; path.write_bytes(data); self.assertNotEqual(original[b'.text'], module.pe_sections(path)[b'.text'])

    def migration_fixture(self):
        gpl = self.root / 'GPL-source.txt'; gpl.write_bytes(b'synthetic license')
        license_record = dict(self.record(gpl.read_bytes()))
        archives = []
        for label in ('legacy', 'monthly'):
            archive = self.root / (label + '.zip'); prefix = label + '-vendor'
            files = {}
            with zipfile.ZipFile(archive, 'w') as handle:
                for name in ('bin/avcodec-62.dll', 'bin/avutil-60.dll', 'bin/swresample-6.dll', 'bin/swscale-9.dll',
                             'LICENSE.txt', 'include/libavcodec/avcodec.h'):
                    body = (label + ':' + name).encode()
                    handle.writestr(prefix + '/' + name, body)
                    files[name] = {'bytes': len(body), 'sha256': hashlib.sha256(body).hexdigest()}
            identity = dict(self.record(archive.read_bytes()), prefix=prefix, source_commit=label + '-source')
            archives.append((archive, identity, files))
        old_archive, old_identity, old_files = archives[0]
        new_archive, new_identity, new_files = archives[1]
        exe = self.root / 'verified-decryptor'; exe.write_bytes(b'known UTF8 working copy')
        config = {'ffmpeg': new_identity, 'ffmpeg_files': new_files,
            'ffmpeg_runtime_files': {name: row for name, row in new_files.items() if not name.startswith('include/')},
            'legacy_ffmpeg': {'identity': old_identity, 'files': old_files}, 'gpl_license': license_record,
            'pspdecrypt': {'known_utf8_sha256': module.sha(exe)}}
        sdk = {'files': [dict(path=name, **row) for name, row in new_files.items() if name.startswith('include/')]}
        data = self.root / '사용자 데이터'; tools = module.own_cache(data, False)
        downloads = tools / 'downloads'; downloads.mkdir()
        shutil.copyfile(old_archive, downloads / 'ffmpeg.zip'); shutil.copyfile(gpl, downloads / 'GPL-3.0.txt')
        legacy = tools / 'ffmpeg'; legacy.mkdir(); module.extract_zip(old_archive, legacy)
        (legacy / 'provenance.json').write_text(json.dumps(old_identity, indent=2))
        shutil.copyfile(gpl, legacy / 'COPYING.GPLv3')
        (data / 'preserve-game-data.txt').write_bytes(b'original user game data')
        return dict(config=config, sdk=sdk, data=data, tools=tools, seed=new_archive, license=gpl, decryptor=exe)

    def migrate(self, fixture, verify_only=False):
        with mock.patch.object(module, 'load_configuration', return_value=(fixture['config'], fixture['sdk'])), \
             mock.patch.object(module, 'installed_sevenzip', return_value=self.root / '7z.exe'), \
             mock.patch.object(module.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            return module.ensure_dependencies(fixture['data'], emit=lambda _: None, verify_only=verify_only,
                decryptor=fixture['decryptor'], ffmpeg_archive=fixture['seed'], license_file=fixture['license'])

    @staticmethod
    def snapshot(folder):
        return {path.relative_to(folder).as_posix(): (path.stat().st_size, module.sha(path))
                for path in folder.rglob('*') if path.is_file()}

    def test_legacy_to_monthly_coexistence_and_both_readonly_verifications(self):
        fixture = self.migration_fixture(); data, config = fixture['data'], fixture['config']
        before = self.snapshot(data); result = self.migrate(fixture)
        prefix = fixture['tools'] / 'ffmpeg' / config['ffmpeg']['sha256'] / config['ffmpeg']['prefix']
        self.assertEqual(result['ffmpeg_prefix'], prefix)
        after = self.snapshot(data)
        self.assertTrue(all(after.get(name) == value for name, value in before.items()))
        self.assertTrue((fixture['tools'] / 'downloads' / ('ffmpeg-' + config['ffmpeg']['sha256'] + '.zip')).is_file())
        self.assertTrue(module.owns_dependency_directory(data))
        self.assertEqual(self.migrate(fixture, verify_only=True), result)
        legacy = module.verify_legacy_ffmpeg(fixture['tools'] / 'ffmpeg', config['legacy_ffmpeg'], config)
        self.assertEqual(legacy.name, config['legacy_ffmpeg']['identity']['prefix'])
        self.assertEqual(self.snapshot(data), after)

    def test_unknown_version_leaf_is_preserved_without_adoption_or_download(self):
        fixture = self.migration_fixture(); config = fixture['config']
        leaf = fixture['tools'] / 'ffmpeg' / config['ffmpeg']['sha256']; leaf.mkdir()
        (leaf / 'foreign.txt').write_bytes(b'preserve unknown files')
        before = self.snapshot(fixture['data'])
        with self.assertRaises(module.DependencyError): self.migrate(fixture)
        self.assertEqual(self.snapshot(fixture['data']), before)

    def test_readonly_upgrade_does_not_treat_legacy_as_new_or_download(self):
        fixture = self.migration_fixture(); before = self.snapshot(fixture['data'])
        with self.assertRaises(module.DependencyError): self.migrate(fixture, verify_only=True)
        self.assertEqual(self.snapshot(fixture['data']), before)

    def test_unknown_codec_container_file_is_preserved_without_adoption(self):
        fixture = self.migration_fixture(); path = fixture['tools'] / 'ffmpeg/user-notes.txt'
        path.write_text('unrelated file'); before = self.snapshot(fixture['data'])
        with self.assertRaises(module.DependencyError): self.migrate(fixture)
        self.assertEqual(self.snapshot(fixture['data']), before)

    def test_tampered_monthly_receipt_files_and_extra_directories_are_preserved(self):
        fixture = self.migration_fixture(); self.migrate(fixture)
        leaf = fixture['tools'] / 'ffmpeg' / fixture['config']['ffmpeg']['sha256']
        for relative, body in [(module.FFMPEG_RECEIPT, b'{}'),
                               (fixture['config']['ffmpeg']['prefix'] + '/bin/avcodec-62.dll', b'wrong DLL'),
                               ('unknown.txt', b'foreign')]:
            source = leaf / relative; existed = source.exists(); original = source.read_bytes() if existed else None
            source.write_bytes(body); before = self.snapshot(fixture['data'])
            with self.subTest(path=relative):
                with self.assertRaises(module.DependencyError): self.migrate(fixture, verify_only=True)
                self.assertEqual(self.snapshot(fixture['data']), before)
            if existed: source.write_bytes(original)
            else: source.unlink()
        (leaf / 'unknown-empty-directory').mkdir()
        with self.assertRaises(module.DependencyError): self.migrate(fixture, verify_only=True)
        self.assertTrue((leaf / 'unknown-empty-directory').is_dir())

    def test_legacy_identity_or_payload_tamper_cannot_be_silently_adopted(self):
        fixture = self.migration_fixture(); container = fixture['tools'] / 'ffmpeg'
        source = container / 'provenance.json'; original = source.read_bytes(); source.write_text('{}')
        before = self.snapshot(fixture['data'])
        with self.assertRaises(module.DependencyError): self.migrate(fixture)
        self.assertEqual(self.snapshot(fixture['data']), before)
        source.write_bytes(original)
        payload = container / fixture['config']['legacy_ffmpeg']['identity']['prefix'] / 'bin/avcodec-62.dll'
        payload.write_bytes(b'unrecognized old DLL'); before = self.snapshot(fixture['data'])
        with self.assertRaises(module.DependencyError): self.migrate(fixture)
        self.assertEqual(self.snapshot(fixture['data']), before)

    def test_failed_new_extraction_never_publishes_or_changes_old_tools_data(self):
        fixture = self.migration_fixture(); before = self.snapshot(fixture['data'])
        fixture['config']['ffmpeg_files']['include/libavcodec/avcodec.h']['sha256'] = '0' * 64
        with self.assertRaises(module.DependencyError): self.migrate(fixture)
        after = self.snapshot(fixture['data'])
        self.assertTrue(all(after.get(name) == value for name, value in before.items()))
        self.assertFalse((fixture['tools'] / 'ffmpeg' / fixture['config']['ffmpeg']['sha256']).exists())
        self.assertFalse(any(path.name.startswith('.ffmpeg-') for path in fixture['tools'].iterdir()))

    def test_fresh_monthly_cache_needs_no_legacy_files(self):
        fixture = self.migration_fixture(); fixture['data'] = self.root / 'fresh data'
        result = self.migrate(fixture)
        self.assertTrue(result['ffmpeg_prefix'].is_dir())
        self.assertFalse((fixture['data'] / 'tools/downloads/ffmpeg.zip').exists())
        before = self.snapshot(fixture['data']); self.migrate(fixture, verify_only=True)
        self.assertEqual(self.snapshot(fixture['data']), before)


if __name__ == '__main__': unittest.main()
