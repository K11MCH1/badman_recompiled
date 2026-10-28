"""Launcher option tests: Skip Intro and the automatic Saves profile."""
import json
from pathlib import Path
import unittest

import test_setup as original

storage = original.backend.player_storage_module()


class SetupUxTests(unittest.TestCase):
    def setUp(self):
        self.fixture = original.SetupTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.fixture.write_data(self.fixture.root)

    def invoke(self):
        f = self.fixture
        return original.backend.setup(f.args, f.identity, f.font_identity, f.events.append, f.runner,
            font_preparer=f.font_preparer, builder=f.builder, dependency_preparer=f.dependencies,
            storage_preparer=lambda root, **options: storage.prepare_player_storage(
                root, active_game_check=lambda: False, **options))

    def files(self):
        root = self.fixture.root
        return {str(p.relative_to(root)): (p.read_bytes(), p.stat().st_mtime_ns) for p in root.rglob('*') if p.is_file()}

    def test_cli_defaults_and_explicit_skip(self):
        f = self.fixture
        common = ['--iso', str(f.iso), '--data-root', str(f.root)]
        args = original.backend.parser().parse_args(common)
        self.assertTrue(args.player_storage)
        self.assertFalse(args.skip_intro)
        self.assertTrue(original.backend.parser().parse_args(common + ['--skip-intro']).skip_intro)
        self.assertFalse(original.backend.parser().parse_args(common + ['--skip-intro', '--no-skip-intro']).skip_intro)

    def test_play_passes_owned_saves_and_skip(self):
        f = self.fixture
        f.args.player_storage = True
        f.args.skip_intro = True
        f.args.play = True
        self.assertEqual(self.invoke(), 1)
        environment = f.native_envs[-1]
        self.assertEqual(environment['BADMAN_SKIP_INTRO'], '1')
        self.assertEqual(Path(environment['BADMAN_SAVES_DIRECTORY']), f.root / 'Saves')
        profile = json.loads((f.root / 'Saves' / storage.PROFILE_NAME).read_text(encoding='utf-8'))
        self.assertFalse(profile['game_payload_created_by_setup'])
        self.assertFalse(list((f.root / 'Saves').rglob('*.bsav')))

    def test_skip_disabled_is_explicit_zero(self):
        f = self.fixture
        f.args.skip_intro = False
        f.args.play = True
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(f.native_envs[-1]['BADMAN_SKIP_INTRO'], '0')
        self.assertNotIn('BADMAN_SAVES_DIRECTORY', f.native_envs[-1])

    def test_verify_with_prepared_storage_is_read_only(self):
        f = self.fixture
        f.args.player_storage = True
        self.invoke()
        before = self.files()
        f.args.verify_only = True
        self.invoke()
        self.assertEqual(before, self.files())

    def test_verify_missing_storage_does_not_create(self):
        f = self.fixture
        self.invoke()  # Prepared game data from an older launcher, without Saves.
        f.args.player_storage = True
        f.args.verify_only = True
        before = self.files()
        with self.assertRaises(original.backend.SetupError) as caught:
            self.invoke()
        self.assertEqual(caught.exception.reason, 'player_storage_not_ready')
        self.assertEqual(before, self.files())
        self.assertFalse((f.root / 'Saves').exists())

    def test_foreign_saves_preserved_and_game_not_started(self):
        f = self.fixture
        (f.root / 'Saves').mkdir()
        (f.root / 'Saves/user-progress').write_bytes(b'user progress')
        f.args.player_storage = True
        f.args.play = True
        with self.assertRaises(original.backend.SetupError) as caught:
            self.invoke()
        self.assertEqual(caught.exception.reason, 'player_storage_preserved')
        self.assertFalse(f.native_envs)
        self.assertEqual((f.root / 'Saves/user-progress').read_bytes(), b'user progress')

    def test_existing_legacy_progress_copied_never_removed(self):
        f = self.fixture
        store = f.root / '.badman-state/memory-stick/BADMAN-NATIVE-V1'
        store.mkdir(parents=True)
        (store / 'OWNER.txt').write_bytes(storage.NATIVE_OWNER)
        (store / 'user-progress.bin').write_bytes(b'existing user progression')
        f.args.player_storage = True
        self.invoke()
        self.assertEqual((store / 'user-progress.bin').read_bytes(), b'existing user progression')
        self.assertEqual((f.root / 'Saves/BADMAN-NATIVE-V1/user-progress.bin').read_bytes(), b'existing user progression')


if __name__ == '__main__':
    unittest.main(verbosity=2)
