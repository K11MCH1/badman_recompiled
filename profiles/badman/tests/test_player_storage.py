"""Synthetic Saves-folder migration tests; no game data or real saves are touched."""
from pathlib import Path
import hashlib,json,os,tempfile,unittest,subprocess,base64
from unittest import mock
import importlib.util
_spec = importlib.util.spec_from_file_location('badman_player_storage_tests', Path(__file__).resolve().parents[1] / 'tools/player_storage.py')
s = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(s)
class StorageTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='badman-player-storage-');self.parent=Path(self.temp.name).resolve();self.root=self.parent/'보유 게임 🎮';self.root.mkdir();self.legacy=self.root/'.badman-state/memory-stick'
    def tearDown(self):
        self.assertEqual(self.parent.parent,Path(tempfile.gettempdir()).resolve());self.assertTrue(self.parent.name.startswith('badman-player-storage-'));self.temp.cleanup()
    def prepare(self,**kw):return s.prepare_player_storage(self.root,active_game_check=lambda:False,**kw)
    def seed(self):
        (self.legacy/'PSP/SAVEDATA').mkdir(parents=True);(self.legacy/'BADMAN-NATIVE-V1').mkdir();(self.legacy/'BADMAN-NATIVE-V1/OWNER.txt').write_bytes(s.NATIVE_OWNER)
        (self.legacy/'BADMAN-NATIVE-V1/ULUS10456-YSave00-YUUSHA.BIN.ref').write_bytes(b'opaque synthetic reference\n')
        (self.legacy/'BADMAN-NATIVE-V1/sha256-synthetic.bsav').write_bytes(b'synthetic existing progress, never fabricated by migration')
        (self.legacy/'PSP/SAVEDATA/PSP-USER-DATA').write_bytes(b'opaque PSP-format data is preserved, not imported')
        (self.legacy/'PICTURE').mkdir();(self.legacy/'PICTURE/사용자.bmp').write_bytes(b'user screenshot bytes')
    def snap(self,p):return {str(q.relative_to(p)):(q.read_bytes(),q.stat().st_mtime_ns)for q in p.rglob('*')if q.is_file()}
    def test_new_silent_host_profile_no_game_payload(self):
        r=self.prepare();self.assertEqual(r['migration'],'new_empty_storage');self.assertEqual(r['profile']['nickname'],'Player');self.assertFalse(r['profile']['game_payload_created_by_setup']);self.assertIsNone(r['profile']['preferred_native_slot']);self.assertFalse(any((self.root/'Saves').rglob('*.bsav')));self.assertTrue((self.root/'Saves/PSP/SAVEDATA').is_dir())
    def test_copy_legacy_preserves_bytes_mtime_and_originals(self):
        self.seed();before=self.snap(self.legacy);r=self.prepare();self.assertEqual(r['migration'],'copied_legacy_originals_retained');self.assertEqual(before,self.snap(self.legacy))
        for name,record in before.items():self.assertEqual((self.root/'Saves'/name).read_bytes(),record[0]);self.assertEqual((self.root/'Saves'/name).stat().st_mtime_ns,record[1])
        receipt=json.loads((self.root/'Saves'/s.RECEIPT_NAME).read_text());self.assertTrue(receipt['source_originals_retained']);self.assertEqual(len(receipt['legacy_files']),len(before))
    def test_relaunch_never_overwrites_new_progress(self):
        self.seed();self.prepare();p=self.root/'Saves/BADMAN-NATIVE-V1/sha256-synthetic.bsav';p.write_bytes(b'newer actual native progress');before=self.snap(self.root/'Saves');self.prepare();self.assertEqual(before,self.snap(self.root/'Saves'));self.assertNotEqual(p.read_bytes(),(self.legacy/p.relative_to(self.root/'Saves')).read_bytes())
    def test_verify_only_missing_does_not_create(self):
        before=self.snap(self.root);r=self.prepare(verify_only=True);self.assertEqual(r['migration'],'not_prepared');self.assertEqual(before,self.snap(self.root));self.assertFalse((self.root/'Saves').exists())
    def test_verify_only_existing_full_tree_unchanged(self):
        self.seed();self.prepare();before=self.snap(self.root);self.prepare(verify_only=True);self.assertEqual(before,self.snap(self.root))
    def test_foreign_destination_preserved(self):
        p=self.root/'Saves';p.mkdir();(p/'valuable').write_bytes(b'user data');before=self.snap(self.root)
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertEqual(before,self.snap(self.root))
    def test_racing_destination_never_replaced(self):
        self.seed();before=self.snap(self.legacy)
        def race(stage,target):target.mkdir();(target/'foreign.txt').write_bytes(b'racing user data')
        with self.assertRaises(s.StorageError):self.prepare(before_publish=race)
        self.assertEqual((self.root/'Saves/foreign.txt').read_bytes(),b'racing user data');self.assertEqual(before,self.snap(self.legacy));self.assertTrue(list(self.root.glob('.badman-saves-stage-*')))
    def test_changing_source_not_published(self):
        self.seed();p=self.legacy/'BADMAN-NATIVE-V1/sha256-synthetic.bsav'
        def change(stage,target):p.write_bytes(b'new concurrent progress')
        with self.assertRaises(s.StorageError):self.prepare(before_publish=change)
        self.assertFalse((self.root/'Saves').exists());self.assertEqual(p.read_bytes(),b'new concurrent progress')
    def test_changing_staged_copy_not_published(self):
        self.seed();before=self.snap(self.legacy)
        def change(stage,target):(stage/'BADMAN-NATIVE-V1/sha256-synthetic.bsav').write_bytes(b'changed staged bytes')
        with self.assertRaises(s.StorageError):self.prepare(before_publish=change)
        self.assertFalse((self.root/'Saves').exists());self.assertEqual(before,self.snap(self.legacy))
    def test_active_game_prevents_any_migration(self):
        self.seed();before=self.snap(self.root)
        with self.assertRaises(s.StorageError):s.prepare_player_storage(self.root,active_game_check=lambda:True)
        self.assertEqual(before,self.snap(self.root))
    def test_game_start_during_copy_preserves_staging(self):
        self.seed();calls=iter((False,True))
        with self.assertRaises(s.StorageError):s.prepare_player_storage(self.root,active_game_check=lambda:next(calls))
        self.assertFalse((self.root/'Saves').exists());self.assertTrue(list(self.root.glob('.badman-saves-stage-*')))
    def test_occupied_commit_lock_preserved(self):
        self.seed();p=self.legacy/'BADMAN-NATIVE-V1/.commit-lock';p.write_bytes(b'foreign or active lock');before=self.snap(self.root)
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertEqual(before,self.snap(self.root))
    def test_occupied_player_lease_preserved(self):
        p=self.root/'.badman-player-session.lock';p.write_bytes(b'foreign lock')
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertEqual(p.read_bytes(),b'foreign lock')
    def test_unknown_native_owner_preserved(self):
        self.seed();(self.legacy/'BADMAN-NATIVE-V1/OWNER.txt').write_bytes(b'unknown');before=self.snap(self.root)
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertEqual(before,self.snap(self.root))
    def test_hardlinked_source_preserved(self):
        self.seed();p=self.legacy/'BADMAN-NATIVE-V1/sha256-synthetic.bsav';os.link(p,self.legacy/'extra-link');before=self.snap(self.root)
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertEqual(before,self.snap(self.root))
    def test_linked_entry_preserved(self):
        self.seed();p=self.legacy/'link'
        try:p.symlink_to(self.parent,target_is_directory=True)
        except OSError:self.skipTest('Windows symlink privilege unavailable')
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertTrue(p.is_symlink());self.assertFalse((self.root/'Saves').exists())
    def test_actual_windows_junction_is_not_followed(self):
        self.seed();target=self.parent/'junction target';target.mkdir();(target/'outside.txt').write_bytes(b'outside the source tree');link=self.legacy/'junction'
        quote=lambda p:"'"+str(p).replace("'","''")+"'"
        source='$null=New-Item -ItemType Junction -Path '+quote(link)+' -Target '+quote(target)+' -ErrorAction Stop'
        result=subprocess.run(['C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-WindowStyle','Hidden','-EncodedCommand',base64.b64encode(source.encode('utf-16-le')).decode('ascii')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(result.returncode,0,(result.stdout,result.stderr));self.assertTrue(link.is_junction())
        try:
            with self.assertRaises(s.StorageError):self.prepare()
            self.assertFalse((self.root/'Saves').exists());self.assertEqual((target/'outside.txt').read_bytes(),b'outside the source tree')
        finally:
            self.assertEqual(link.resolve(),target.resolve());self.assertTrue(link.is_junction());os.rmdir(link)
    def test_reserved_metadata_collision_preserved(self):
        self.seed();(self.legacy/s.PROFILE_NAME).write_bytes(b'user data');before=self.snap(self.root)
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertEqual(before,self.snap(self.root))
    def test_profile_corruption_is_not_silently_reset(self):
        self.prepare();p=self.root/'Saves'/s.PROFILE_NAME;p.write_bytes(b'corrupt but preserved')
        with self.assertRaises(s.StorageError):self.prepare()
        self.assertEqual(p.read_bytes(),b'corrupt but preserved')
if __name__=='__main__':unittest.main(verbosity=2)
