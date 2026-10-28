"""Prepare the user's ISO, generate/compile its local native build, and optionally Play.

Uses verified external preparation tools and the bundled runtime sources.
All process arguments are passed as lists. A ready setup is not a playable-port claim.
"""
import argparse
import functools
from datetime import datetime, timezone
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib

PROFILE = Path(__file__).resolve().parents[1]
ROOT = PROFILE.parent.parent if PROFILE.parent.name.casefold() == 'profiles' else PROFILE
MANIFEST = 'badman-setup.json'
PRODUCER = 'PSPRecomp Badman setup'
ELF = 'PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF'
MODULE = 'PSP_GAME/USRDIR/data_usa/module/libfont.prx'
SFO = 'PSP_GAME/PARAM.SFO'


class SetupError(Exception):
    def __init__(self, reason, message):
        self.reason = reason
        super().__init__(message)


def digest(path, emit=None):
    """Bounded memory, at most eleven progress updates per file."""
    size = path.stat().st_size
    result = hashlib.sha256()
    done, next_percent = 0, 0
    with path.open('rb') as handle:
        while chunk := handle.read(1024 * 1024):
            result.update(chunk)
            done += len(chunk)
            percent = min(100, done * 100 // max(size, 1))
            if emit and percent >= next_percent:
                emit(f'Checking ISO: {percent}% ({done:,}/{size:,} bytes)')
                next_percent = (percent // 10 + 1) * 10
    return result.hexdigest()


def parse_sfo(data):
    try:
        magic, version, keys, values, count = struct.unpack_from('<4sIIII', data)
        if magic != b'\0PSF' or version != 0x101 or count > 128 or 20 + count * 16 > len(data):
            raise ValueError('invalid PSF header')
        result = {}
        for index in range(count):
            key_offset, kind, length, maximum, offset = struct.unpack_from('<HHIII', data, 20 + index * 16)
            key_start, value_start = keys + key_offset, values + offset
            if not 0 <= key_start < len(data) or value_start + length > len(data) or length > maximum:
                raise ValueError('PSF entry outside file')
            key_end = data.index(0, key_start, min(len(data), key_start + 128))
            key = data[key_start:key_end].decode('ascii')
            if kind == 0x204:
                result[key] = data[value_start:value_start + length].split(b'\0', 1)[0].decode('utf-8')
        return result
    except (ValueError, UnicodeError, struct.error) as error:
        raise SetupError('invalid_disc_metadata', f'Invalid PARAM.SFO: {error}') from error


def linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def data_file(root, relative):
    path = root / relative
    if not path.is_file() or linked(path) or not path.resolve().is_relative_to(root):
        raise SetupError('data_not_ready', f'Missing or unsafe prepared file: {path}')
    return path


def validate_data(root, identity, font_identity):
    elf, module, sfo = (data_file(root, relative) for relative in (ELF, MODULE, SFO))
    with elf.open('rb') as handle:
        magic = handle.read(4)
    if magic != b'\x7fELF':
        raise SetupError('invalid_executable', 'Prepared EBOOT_DECRYPTED.ELF is not an ELF executable')
    result = {}
    for key, path, expected in [('elf', elf, identity['elf_sha256']), ('module', module, font_identity['input_sha256'])]:
        actual = digest(path)
        if actual != expected:
            raise SetupError('unsupported_' + key, f'{path.name} hash does not match the supported {identity["disc_id"]} {identity["disc_version"]} input')
        result[key] = {'path': path.relative_to(root).as_posix(), 'sha256': actual, 'size': path.stat().st_size}
    if sfo.stat().st_size > 1024 * 1024:
        raise SetupError('invalid_disc_metadata', 'PARAM.SFO is too large')
    metadata = parse_sfo(sfo.read_bytes())
    if metadata.get('DISC_ID') != identity['disc_id'] or metadata.get('DISC_VERSION') != identity['disc_version']:
        raise SetupError('unsupported_disc', f'Expected {identity["disc_id"]} disc version {identity["disc_version"]}; found {metadata.get("DISC_ID")} {metadata.get("DISC_VERSION")}')
    result['sfo'] = {'path': SFO, 'sha256': digest(sfo), 'size': sfo.stat().st_size}
    return result


def run_tool(command, emit, capture=False, env=None):
    process = subprocess.Popen([str(value) for value in command], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', shell=False,
                               env=env)
    captured, captured_size = [], 0
    assert process.stdout is not None
    try:
        while line := process.stdout.readline(65537):
            if len(line) > 65536:
                raise SetupError('tool_output_invalid', 'External tool produced an oversized output line')
            if capture:
                captured_size += len(line)
                if captured_size > 8 * 1024 * 1024:
                    raise SetupError('tool_output_invalid', 'External archive listing exceeded the bounded output size')
                captured.append(line)
            else:
                emit(line.rstrip('\r\n'))
        return process.wait(), ''.join(captured)
    except BaseException:
        process.kill()
        process.wait()
        raise
    finally:
        process.stdout.close()


def validate_archive_listing(text):
    entries, seen = 0, set()
    in_entries = False
    for line in text.splitlines():
        if line.startswith('----------'):
            in_entries = True
        if not in_entries:
            continue
        if line.startswith(('Symbolic Link = ', 'Hard Link = ')) and line.partition('=')[2].strip():
            raise SetupError('unsafe_archive', 'Archive contains links')
        if not line.startswith('Path = '):
            continue
        name = line[7:].replace('\\', '/')
        parts = PurePosixPath(name).parts
        if (not parts or name.startswith('/') or ':' in name or any(part in ('', '.', '..') or
                part.endswith((' ', '.')) for part in parts)):
            raise SetupError('unsafe_archive', f'Unsafe archive entry: {name}')
        if parts[0] != 'PSP_GAME':
            continue
        key = name.casefold()
        if key in seen:
            raise SetupError('unsafe_archive', f'Duplicate archive destination: {name}')
        seen.add(key)
        entries += 1
        if entries > 100000:
            raise SetupError('unsafe_archive', 'Too many archive entries')
    required = {SFO.casefold(), 'psp_game/sysdir/eboot.bin', MODULE.casefold()}
    if not required.issubset(seen):
        raise SetupError('invalid_archive', 'ISO listing lacks required PSP_GAME metadata, EBOOT or libfont files')
    return entries


def cleanup_stage(stage, parent):
    resolved = stage.resolve()
    if linked(stage) or resolved.parent != parent.resolve() or not resolved.name.startswith('.badman-setup-'):
        raise SetupError('unsafe_cleanup', f'Refusing cleanup outside the verified staging parent: {stage}')
    shutil.rmtree(resolved)


def read_manifest(root):
    path = root / MANIFEST
    if not path.exists():
        return None
    try:
        if linked(path) or not path.is_file():
            raise ValueError('unsafe manifest path')
        data = json.loads(path.read_text(encoding='utf-8'))
        if (data.get('producer') != PRODUCER or data.get('schema') not in (1, 2, 3) or data.get('state') != 'ready' or
                not isinstance(data.get('iso'), dict) or not isinstance(data.get('data'), dict)):
            raise ValueError('unrecognized manifest owner/schema/state')
        return data
    except (ValueError, OSError) as error:
        raise SetupError('occupied_data_root', f'Preserving unrecognized {path}: {error}') from error


def write_manifest(root, manifest):
    target = root / MANIFEST
    if target.exists():
        read_manifest(root)  # Do not overwrite an unrelated file at our manifest name.
    temporary = root / ('.badman-setup-' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8', newline='\n') as handle:
            json.dump(manifest, handle, indent=2)
            handle.write('\n')
        temporary.replace(target)
    finally:
        if temporary.exists() and temporary.parent.resolve() == root and temporary.name.startswith('.badman-setup-'):
            temporary.unlink()


def iso_stat(path):
    stat = path.stat()
    return {'path': str(path), 'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns,
            'device': stat.st_dev, 'inode': stat.st_ino}


def prepared_font_path(root, identity):
    expected = identity['prepared_sha256']
    if len(expected) != 64 or any(character not in '0123456789abcdef' for character in expected):
        raise SetupError('invalid_profile_config', 'Invalid prepared font identity')
    for folder in (root / 'prepared', root / 'prepared' / expected):
        if linked(folder) or (folder.exists() and not folder.is_dir()):
            raise SetupError('occupied_prepared_font', f'Preserving unsafe/unrelated prepared-font location: {folder}')
    return root / 'prepared' / expected / 'libfont-fixed.elf'


def checked_fixed_font(root, target, identity):
    if not target.exists():
        return False
    if (linked(target) or not target.is_file() or not target.resolve().is_relative_to(root) or
            target.stat().st_size > 1024 * 1024 or digest(target) != identity['prepared_sha256']):
        raise SetupError('occupied_prepared_font', f'Preserving an unrelated or invalid prepared font artifact: {target}')
    return True


def prepare_font_module(root, identity, decryptor, emit, runner, verify_only=False, reconstructor=None):
    """Prepare only this user's hash-locked PRX using external cryptography."""
    target = prepared_font_path(root, identity)
    if checked_fixed_font(root, target, identity):
        emit('Verified prepared native font module; reusing its matching artifact.')
        return target
    if verify_only:
        raise SetupError('font_not_ready', 'Native font artifact is missing; run Setup to prepare it from the selected ISO')
    if not decryptor.is_file():
        raise SetupError('missing_decryptor', f'External pspdecrypt is required to prepare native font data: {decryptor}')
    raw_file = data_file(root, MODULE)
    if raw_file.stat().st_size > 1024 * 1024:
        raise SetupError('unsupported_module', 'Font module exceeds the supported preparation size')
    raw = raw_file.read_bytes()
    if hashlib.sha256(raw).hexdigest() != identity['input_sha256']:
        raise SetupError('unsupported_module', 'Font source changed or does not match the supported module')
    if len(raw) < 68 or raw[:4] != b'~SCE' or struct.unpack_from('<I', raw, 4)[0] != 64 or raw[64:68] != b'~PSP':
        raise SetupError('font_envelope_invalid', 'Unexpected native font wrapper; original input was preserved')
    stage = Path(tempfile.mkdtemp(prefix='.badman-setup-font-', dir=root.parent)).resolve()
    try:
        inner, compressed = stage / 'libfont-psp.prx', stage / 'libfont-decrypted.bin'
        inner.write_bytes(raw[64:])
        emit('Preparing the ISO font module with the external decryption tool.')
        code, _ = runner([decryptor, '-o', compressed, inner], emit)
        if code:
            raise SetupError('font_decryption_failed', f'External font decryption failed (exit {code})')
        payload = data_file(stage, compressed.name)
        if payload.stat().st_size > 1024 * 1024:
            raise SetupError('font_unpack_failed', 'Decrypted font container exceeds the bounded preparation size')
        packed = payload.read_bytes()
        if packed[:2] != b'\x1f\x8b':
            raise SetupError('font_unpack_failed', 'Font decryptor did not produce the expected gzip container')
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(packed), mode='rb') as stream:
                unpacked = stream.read(35302 + 1)
        except (OSError, EOFError, zlib.error) as error:
            raise SetupError('font_unpack_failed', f'Invalid compressed font module: {error}') from error
        if (len(unpacked) != 35302 or unpacked[:4] != b'\x7fELF' or
                hashlib.sha256(unpacked).hexdigest() != identity['unpacked_sha256']):
            raise SetupError('font_unpack_failed', 'Unpacked font size, ELF magic or SHA256 does not match the supported module')
        unpacked_file, fixed = stage / 'libfont-unpacked.elf', stage / 'libfont-fixed.elf'
        unpacked_file.write_bytes(unpacked)
        if reconstructor is None:
            path = Path(__file__).with_name('reconstruct_font.py')
            reconstruction = load_helper(path, 'badman_setup_font_reconstruction')
            reconstructor = reconstruction.reconstruct_file
        emit('Reconstructing the verified native font module at its compiled load address.')
        try:
            reconstructor(unpacked_file, fixed)
        except (ValueError, OSError, struct.error) as error:
            raise SetupError('font_reconstruction_failed', str(error)) from error
        if digest(fixed) != identity['prepared_sha256']:
            raise SetupError('font_reconstruction_failed', 'Prepared font hash differs from the compiled translation')
        prepared_font_path(root, identity)  # Recheck before creating/committing destination.
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.parent.resolve().is_relative_to(root):
            raise SetupError('occupied_prepared_font', 'Prepared-font destination escaped the data root')
        if not checked_fixed_font(root, target, identity):
            try:
                if os.name == 'nt':
                    fixed.rename(target)  # Windows rename atomically refuses an existing destination.
                else:
                    os.link(fixed, target)  # Same-volume atomic no-overwrite publish.
            except FileExistsError:
                checked_fixed_font(root, target, identity)
        if not checked_fixed_font(root, target, identity):
            raise SetupError('font_reconstruction_failed', 'Prepared artifact was not committed')
        emit('Native font module prepared and its final hash verified.')
        return target
    finally:
        cleanup_stage(stage, root.parent)


def load_helper(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    # SourceFileLoader.exec_module may write __pycache__ even in verify-only.
    # Trusted shipped helper sources can be compiled in memory without that IO.
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def prepare_native_build(data_root, elf, font_elf, source_root=None, ffmpeg_prefix=None, cmake=None,
                         jobs=2, emit=print, verify_only=False):
    """Load the bundled source-build orchestrator; never select a prebuilt EXE."""
    path = Path(__file__).with_name('build_game.py')
    if not path.is_file():
        raise SetupError('build_backend_missing', f'Local generation/build tool is missing: {path}')
    module = load_helper(path, 'badman_local_build_backend')
    try:
        return module.ensure_build(data_root, elf, font_elf, source_root=source_root,
            ffmpeg_prefix=ffmpeg_prefix, cmake=cmake, jobs=jobs, emit=emit, verify_only=verify_only)
    except module.BuildError as error:
        raise SetupError(getattr(error, 'reason', 'local_build_failed'), str(error)) from error


@functools.cache
def player_storage_module():
    # One shared instance keeps StorageError identity stable for callers.
    return load_helper(Path(__file__).with_name('player_storage.py'), 'badman_player_storage')


def dependency_module():
    path = Path(__file__).with_name('toolchain_bootstrap.py')
    if not path.is_file():
        raise SetupError('dependency_bootstrap_missing', f'Verified dependency preparation tool is missing: {path}')
    return load_helper(path, 'badman_setup_dependencies')


def owns_dependencies(root):
    try:
        return dependency_module().owns_dependency_directory(root)
    except (SetupError, OSError, ValueError):
        return False


def prepare_dependencies(data_root, emit=print, verify_only=False, sevenzip=None,
                         decryptor=None, ffmpeg_prefix=None):
    module = dependency_module()
    try:
        return module.ensure_dependencies(data_root, emit=emit, verify_only=verify_only,
            sevenzip=sevenzip, decryptor=decryptor, ffmpeg_prefix=ffmpeg_prefix)
    except module.DependencyError as error:
        raise SetupError(error.reason, str(error)) from error


def checked_native_build(root, target):
    build_root = root / 'build'
    target = Path(target).expanduser()
    if not target.is_absolute() or linked(build_root) or not target.resolve().is_relative_to(build_root.resolve()):
        raise SetupError('unsafe_native_build', 'Local native build must be inside this data folder\'s owned build cache')
    cursor = target
    while cursor != build_root:
        if linked(cursor) or cursor.parent == cursor:
            raise SetupError('unsafe_native_build', 'Local native build cannot contain linked or escaping paths')
        cursor = cursor.parent
    if not target.is_file() or target.suffix.casefold() != '.exe' or target.stat().st_nlink != 1:
        raise SetupError('native_build_not_ready', f'Verified local native executable is missing or unsafe: {target}')
    with target.open('rb') as handle:
        if handle.read(2) != b'MZ':
            raise SetupError('native_build_not_ready', 'Local compiled artifact is not a Windows executable')
    return target.resolve()


def setup(args, identity=None, font_identity=None, emit=print, runner=run_tool,
          font_preparer=prepare_font_module, builder=prepare_native_build,
          dependency_preparer=prepare_dependencies, storage_preparer=None):
    renderer = getattr(args, 'renderer', 'd3d11')
    renderer_labels = {'d3d11': 'Direct3D 11', 'vulkan': 'Vulkan', 'software': 'Software'}
    if not isinstance(renderer, str) or renderer not in renderer_labels:
        raise SetupError('invalid_renderer', 'Choose a Rendering API: d3d11, vulkan or software')
    identity = identity or json.loads((PROFILE / 'config/executable.json').read_text())
    font_identity = font_identity or json.loads((PROFILE / 'config/font_module.json').read_text())
    try:
        expected_iso, expected_size = identity['iso_sha256'], identity['iso_size']
    except KeyError as error:
        raise SetupError('invalid_profile_config', 'Profile executable.json must identify the supported ISO hash and size') from error
    iso = args.iso.expanduser().resolve()
    if not iso.is_file() or iso.suffix.lower() != '.iso':
        raise SetupError('iso_not_found', 'Select an existing .iso file')
    if iso.stat().st_size != expected_size:
        raise SetupError('unsupported_iso', f'ISO size is unsupported; expected {expected_size:,} bytes for {identity["disc_id"]} {identity["disc_version"]}')
    original_root = args.data_root.expanduser().absolute()
    if linked(original_root):
        raise SetupError('unsafe_data_root', 'Data folder cannot be a symbolic link or junction')
    root = original_root.resolve()
    if root.exists() and not root.is_dir():
        raise SetupError('occupied_data_root', 'Data folder points to an existing file')
    existing = root.exists() and any(root.iterdir())
    previous = read_manifest(root) if existing else None
    checked = None
    if existing:
        if {path.name for path in root.iterdir()} == {'tools'} and owns_dependencies(root):
            emit('Resuming preparation with the verified owned dependency cache; no game files are installed yet.')
        else:
            emit('Checking existing prepared game data; all unrelated files will be preserved.')
            try:
                checked = validate_data(root, identity, font_identity)
            except SetupError as error:
                raise SetupError('occupied_data_root', f'Preserving occupied data folder: {error}. Choose a new empty folder to prepare this ISO.') from error
    stat_before = iso_stat(iso)
    cached = (not args.verify_only and previous and previous.get('iso') == {**stat_before, 'sha256': expected_iso}
              and all(previous['data'].get(key) == value for key, value in (checked or {}).items()))
    if cached:
        emit('Reusing recorded ISO verification (unchanged source path/stat); prepared ELF, libfont and disc metadata hashes verified.')
    else:
        emit('Verifying supported ISO SHA256 before preparation.')
        if digest(iso, emit) != expected_iso or iso_stat(iso) != stat_before:
            raise SetupError('unsupported_iso', 'ISO hash changed or does not match the supported image; the ISO and data were left untouched')
    if args.verify_only and checked is None:
        raise SetupError('data_not_ready', 'No matching prepared game data exists; run Setup before verification')
    dependencies = dependency_preparer(root, emit=emit, verify_only=args.verify_only,
        sevenzip=getattr(args, 'sevenzip', None), decryptor=getattr(args, 'decryptor', None),
        ffmpeg_prefix=getattr(args, 'ffmpeg_prefix', None))
    try:
        sevenzip, decryptor, ffmpeg_prefix = (Path(dependencies[key]).expanduser().resolve()
                                            for key in ('sevenzip', 'decryptor', 'ffmpeg_prefix'))
    except (KeyError, TypeError) as error:
        raise SetupError('invalid_dependency_result', 'Verified dependency preparation did not return required tool paths') from error
    if checked is None:
        if not sevenzip.is_file():
            raise SetupError('missing_sevenzip', f'7-Zip is required for ISO extraction: {sevenzip}')
        if not decryptor.is_file():
            raise SetupError('missing_decryptor', f'External pspdecrypt is required to prepare EBOOT: {decryptor}')
        emit('Checking archive paths before extraction.')
        code, listing = runner([sevenzip, 'l', '-slt', iso], emit, capture=True)
        if code:
            raise SetupError('archive_listing_failed', f'7-Zip could not read the selected ISO (exit {code})')
        emit(f'Archive path check passed ({validate_archive_listing(listing)} PSP_GAME entries).')
        root.parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix='.badman-setup-', dir=root.parent)).resolve()
        try:
            emit('Extracting PSP_GAME into a new staging folder; bundled UPDATE is excluded.')
            code, _ = runner([sevenzip, 'x', '-y', '-bsp0', '-o' + str(stage), '-xr!PSP_GAME/SYSDIR/UPDATE', iso, 'PSP_GAME/*'], emit)
            if code:
                raise SetupError('extraction_failed', f'7-Zip extraction failed (exit {code})')
            for path in stage.rglob('*'):
                if linked(path) or not path.resolve().is_relative_to(stage):
                    raise SetupError('unsafe_archive', 'Extracted data contains a link or escaping path')
            if (stage / 'PSP_GAME/SYSDIR/UPDATE').exists():
                raise SetupError('updater_not_excluded', '7-Zip did not exclude the bundled updater; staged data will not be installed')
            encrypted = data_file(stage, 'PSP_GAME/SYSDIR/EBOOT.BIN')
            output = stage / ELF
            emit('Decrypting the game executable with the external preparation tool.')
            code, _ = runner([decryptor, '-o', output, encrypted], emit)
            if code:
                raise SetupError('decryption_failed', f'External pspdecrypt failed (exit {code})')
            checked = validate_data(stage, identity, font_identity)
            if iso_stat(iso) != stat_before:
                raise SetupError('iso_changed', 'Selected ISO changed during extraction; staged data will not be installed')
            if root.exists() and any(root.iterdir()):
                if {path.name for path in root.iterdir()} != {'tools'} or not owns_dependencies(root):
                    raise SetupError('occupied_data_root', 'Data folder changed during setup; preserving its contents')
            root.mkdir(exist_ok=True)
            (stage / 'PSP_GAME').rename(root / 'PSP_GAME')
        finally:
            cleanup_stage(stage, root.parent)
    assert checked is not None
    fixed_font = font_preparer(root, font_identity, decryptor, emit, runner,
                              verify_only=args.verify_only)
    if not checked_fixed_font(root, fixed_font, font_identity):
        raise SetupError('font_not_ready', 'Native font artifact is not ready')
    checked['fixed_font'] = {'path': fixed_font.relative_to(root).as_posix(),
                             'sha256': font_identity['prepared_sha256'], 'size': fixed_font.stat().st_size}
    if args.font_directory:
        fonts = args.font_directory.expanduser().resolve()
        if not fonts.is_dir() or not any(fonts.glob('*.pgf')):
            raise SetupError('font_directory_invalid', 'Selected font directory must contain PGF files')
    else:
        fonts = None
    def check_source_unchanged():
        try:
            source_changed = iso_stat(iso) != stat_before
        except OSError as error:
            raise SetupError('iso_changed', 'Selected ISO became unavailable during preparation; readiness was not updated') from error
        if source_changed:
            raise SetupError('iso_changed', 'Selected ISO changed during preparation; readiness was not updated')
    check_source_unchanged()
    build_root = root / 'build'
    if linked(build_root) or (build_root.exists() and not build_root.is_dir()):
        raise SetupError('unsafe_build_root', 'Preserving linked or non-directory local build cache')
    emit('Verifying the local native build.' if args.verify_only else
         'Generating and compiling your local native game build; the first build can take several minutes.')
    native = checked_native_build(root, builder(root, root / ELF, fixed_font,
        source_root=getattr(args, 'source_root', None), ffmpeg_prefix=ffmpeg_prefix,
        cmake=getattr(args, 'cmake', None), jobs=getattr(args, 'jobs', 2), emit=emit, verify_only=args.verify_only))
    checked['compiled_native'] = {'path': native.relative_to(root).as_posix(),
                                  'sha256': digest(native), 'size': native.stat().st_size}
    # Build time is part of preparation. Recheck both source identity and every
    # runtime input before publishing ready, including on a cached build.
    check_source_unchanged()
    validate_data(root, identity, font_identity)
    if not checked_fixed_font(root, fixed_font, font_identity):
        raise SetupError('font_not_ready', 'Prepared font disappeared during the local build')
    if not args.verify_only:
        write_manifest(root, {'producer': PRODUCER, 'schema': 3, 'state': 'ready',
            'disc_id': identity['disc_id'], 'disc_version': identity['disc_version'],
            'iso': {**stat_before, 'sha256': expected_iso}, 'data': checked,
            'validated': 'Supported ISO, disc metadata, decrypted ELF, prepared font and locally generated/compiled native build',
            'prepared_at': datetime.now(timezone.utc).isoformat()})
    storage = None
    if getattr(args, 'player_storage', False):
        # The Saves folder and its default host profile are created silently.
        # No game payload is invented; the game's own Save fills the profile.
        storage_module = player_storage_module()
        storage_preparer = storage_preparer or storage_module.prepare_player_storage
        try:
            storage = storage_preparer(root, verify_only=args.verify_only)
        except storage_module.StorageError as error:
            raise SetupError('player_storage_preserved', str(error)) from error
        if args.verify_only and storage['migration'] == 'not_prepared':
            raise SetupError('player_storage_not_ready', 'Player storage is not prepared; Prepare first. Verification wrote nothing.')
        emit('Saves folder: ' + storage['saves'])
        if storage['migration'] == 'copied_legacy_originals_retained':
            emit('Existing save files copied and verified. Original storage remains unchanged for recovery.')
    emit(f'READY: {identity["disc_id"]} {identity["disc_version"]}; verified local build: {native}')
    art_directory = getattr(args, 'art_directory', None)
    if art_directory and not args.verify_only and not args.play:
        prepare_title_art(Path(art_directory), native, root, fonts, fixed_font, storage, renderer, emit, runner)
    if not args.play:
        return 0
    log_directory = root / 'launcher-logs'
    if linked(log_directory):
        raise SetupError('unsafe_log_path', 'Launcher log directory cannot be a link')
    log_directory.mkdir(exist_ok=True)
    session = str(time.time_ns()) + '-' + uuid.uuid4().hex[:8]
    trace = log_directory / (session + '-hle.log') if args.trace else Path(os.devnull)
    command = [native, root / ELF, root, str(args.dispatch_limit), trace, fonts if fonts else '', fixed_font]
    child_env = os.environ.copy()
    child_env['BADMAN_RENDERER'] = renderer
    child_env['BADMAN_SKIP_INTRO'] = '1' if getattr(args, 'skip_intro', False) else '0'
    if storage is not None:
        child_env['BADMAN_SAVES_DIRECTORY'] = storage['saves']
    with (log_directory / (session + '-session.log')).open('x', encoding='utf-8', newline='\n') as log:
        def native_output(line):
            emit(line)
            log.write(line + '\n')
            log.flush()
        emit(f'Starting native game with {renderer_labels[renderer]}; current implementation limits will be reported below.')
        emit('Session log: ' + str(log.name))
        try:
            code, _ = runner(command, native_output, env=child_env)
        except OSError as error:
            raise SetupError('native_launch_failed', f'Cannot start native executable with {renderer_labels[renderer]}: {error}') from error
        if code:
            native_output(f'Native session stopped (exit {code}) using {renderer_labels[renderer]}. See the session log for details. '
                          'If this backend is unavailable, choose a different Rendering API and Play again.')
        return code


def prepare_title_art(art_directory, native, root, fonts, fixed_font, storage, renderer, emit, runner):
    """Capture this player's own title screen once for the launcher background.

    The frame comes from the locally compiled game; nothing is distributed.
    Failures leave the launcher's plain background and never fail preparation.
    """
    target = art_directory / 'play.png'
    if target.is_file():
        return
    try:
        art_directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='badman-title-art-') as directory:
            work = Path(directory)
            control, frame, staged = work / 'control.txt', work / 'title.bmp', work / 'play.png'
            control.write_text('0 0 0\n', encoding='ascii')
            environment = os.environ.copy()
            environment.update({'BADMAN_HEADLESS': '1', 'BADMAN_RUN_SECONDS': '30', 'BADMAN_SKIP_INTRO': '1',
                                'BADMAN_INPUT_CONTROL_FILE': str(control), 'BADMAN_DUMP_FRAME': str(frame),
                                'BADMAN_RENDERER': renderer})
            if storage is not None:
                environment['BADMAN_SAVES_DIRECTORY'] = storage['saves']
            emit('Capturing your title screen for the launcher background (about 30 seconds).')
            runner([native, root / ELF, root, str((1 << 64) - 1), Path(os.devnull), fonts if fonts else '', fixed_font],
                   lambda line: None, env=environment)
            if not frame.is_file():
                raise OSError('the game did not produce a title frame')
            load_helper(Path(__file__).with_name('launcher_art.py'), 'badman_launcher_art').title_art(frame, staged)
            os.replace(staged, target)
        emit('Launcher title background prepared from your own game.')
    except Exception as error:  # Optional artwork; preparation already succeeded.
        emit(f'Launcher title background was not prepared ({error}); the plain background stays.')


def positive(value):
    number = int(value)
    if not 0 < number <= 0xffffffffffffffff:
        raise argparse.ArgumentTypeError('dispatch limit must be a positive 64-bit integer')
    return number


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--iso', type=Path, required=True)
    result.add_argument('--data-root', type=Path, required=True)
    result.add_argument('--font-directory', type=Path, default=PROFILE / 'assets/fonts' if (PROFILE / 'assets/fonts').is_dir() else None)
    result.add_argument('--dispatch-limit', type=positive, default=(1 << 64) - 1,
                        help='Optional diagnostic execution cap; normal play has no practical cap')
    result.add_argument('--trace', action='store_true')
    result.add_argument('--play', action='store_true')
    result.add_argument('--renderer', choices=('d3d11', 'vulkan', 'software'), default='d3d11',
                        help='Rendering API used by Play (default: d3d11); preparation builds all backends')
    result.add_argument('--skip-intro', action=argparse.BooleanOptionalAction, default=False,
                        help='Answer the boot Memory Stick prompt and skip the logo movie')
    result.add_argument('--player-storage', action=argparse.BooleanOptionalAction, default=True,
                        help='Prepare the Saves folder and its automatic default profile (default: on)')
    result.add_argument('--art-directory', type=Path, help='Launcher art folder; Prepare captures the title background there once')
    result.add_argument('--verify-only', action='store_true')
    result.add_argument('--source-root', type=Path, help='Runtime source checkout; defaults to bundled runtime-source')
    result.add_argument('--ffmpeg-prefix', type=Path, help='Pinned shared decoder dependency directory')
    result.add_argument('--cmake', type=Path, help='Explicit CMake executable for local compilation')
    result.add_argument('--jobs', type=positive, default=2, help='Parallel compiler processes (default: 2)')
    result.add_argument('--sevenzip', type=Path, help='Verified installed 7-Zip override')
    result.add_argument('--decryptor', type=Path, help='Hash-verified development decryptor override')
    return result


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
    args = parser().parse_args()
    if args.verify_only and args.play:
        parser().error('--verify-only and --play cannot be combined')
    try:
        return setup(args, emit=lambda line: print(line, flush=True))
    except SetupError as error:
        print(f'ERROR [{error.reason}]: {error}', file=sys.stderr, flush=True)
        return 1
    except (OSError, ValueError) as error:
        print(f'ERROR [setup_failed]: {error}', file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__':
    sys.exit(main())
