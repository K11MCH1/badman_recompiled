"""Fetch pinned external dependencies into the player's owned local tool cache.

The distribution contains only dependency facts, never these vendor binaries.
Downloads, extraction and the UTF-8 resource change happen on the user's machine.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import urllib.request
import uuid
import xml.etree.ElementTree as ET
import zipfile

BASE = Path(__file__).resolve().parent.parent
PRODUCER = 'PSPRecomp verified local dependency cache'
OWNER = '.psprecomp-tools-owner.json'
FFMPEG_RECEIPT = '.psprecomp-ffmpeg-version.json'
STAGE_MAX_ENTRIES = 10000
STAGE_MAX_BYTES = 1024 * 1024 * 1024
STAGE_MAX_DEPTH = 32
CACHE_LOCK_BYTE = 16 * 1024 * 1024


class DependencyError(Exception):
    def __init__(self, reason, message):
        self.reason = reason
        super().__init__(message)


def sha(path):
    with Path(path).open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def fail(reason, message):
    raise DependencyError(reason, message)


def load_configuration():
    config = json.loads((BASE / 'config/toolchain_dependencies.json').read_text(encoding='utf-8'))
    sdk = json.loads((BASE / 'config/ffmpeg_sdk_files.json').read_text(encoding='utf-8'))
    if (config.get('schema') != 1 or sdk.get('archive_sha256') != config['ffmpeg']['sha256'] or
            sdk.get('source_commit') != config['ffmpeg']['source_commit']):
        fail('invalid_dependency_configuration', 'Dependency archive and SDK identities disagree')
    files = config.get('ffmpeg_files', {})
    if not files or any(files.get(row['path']) != {'bytes': row['bytes'], 'sha256': row['sha256']} for row in sdk['files']):
        fail('invalid_dependency_configuration', 'Exact vendor inventory does not cover the pinned public SDK')
    if any(files.get(name) != record for name, record in config['ffmpeg_runtime_files'].items()):
        fail('invalid_dependency_configuration', 'Exact vendor inventory does not cover the pinned codec runtime')
    return config, sdk


def verify_file(path, record, label):
    path = Path(path)
    if linked(path) or not path.is_file() or path.stat().st_size != record['bytes'] or sha(path) != record['sha256']:
        fail('dependency_identity_mismatch', 'Verified dependency is missing or changed: ' + label)
    return path


def member_name(name):
    path = PurePosixPath(name)
    reserved = {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)), *(f'LPT{i}' for i in range(1, 10))}
    if (not name or '\\' in name or ':' in name or path.is_absolute() or not path.parts or
        any(part in {'.', '..'} or part.endswith((' ', '.')) or part.split('.')[0].upper() in reserved or
            any(ord(char) < 32 for char in part) for part in path.parts)):
        fail('unsafe_dependency_archive', 'Unsafe dependency archive member: ' + name)
    return path


def interrupted_stage(path):
    """Recognize a bounded abandoned workspace, never trusted/reused/deleted.

    These are the exact tempfile names used by both old and current bootstraps.
    A name is only permission to preserve/ignore a link-free bounded tree, not
    evidence that its contents are usable dependencies or safe to remove.
    """
    path = Path(path)
    if not re.fullmatch(r'\.(ffmpeg|pspdecrypt)-[a-z0-9_]{8}', path.name): return False
    if linked(path) or not path.is_dir(): return False
    pending, count, size = [(path, 0)], 0, 0
    try:
        while pending:
            folder, depth = pending.pop()
            if depth > STAGE_MAX_DEPTH: return False
            with os.scandir(folder) as entries:
                for entry in entries:
                    child = Path(entry.path)
                    count += 1
                    if count > STAGE_MAX_ENTRIES or linked(child): return False
                    member_name(child.relative_to(path).as_posix())
                    info = child.stat(follow_symlinks=False)
                    if stat.S_ISDIR(info.st_mode): pending.append((child, depth + 1))
                    elif stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
                        size += info.st_size
                        if size > STAGE_MAX_BYTES: return False
                    else: return False
        return True
    except (OSError, ValueError): return False


def owns_dependency_directory(data_root):
    root = Path(data_root).absolute(); folder = root / 'tools'
    try:
        if linked(root) or linked(folder) or not folder.is_dir() or folder.resolve().parent != root.resolve(): return False
        marker = folder / OWNER
        if linked(marker) or not marker.is_file() or marker.stat().st_size > 4096: return False
        record = json.loads(marker.read_text(encoding='utf-8'))
        if record != {'producer': PRODUCER, 'schema': 1}: return False
        stages = 0
        for index, path in enumerate(folder.iterdir()):
            if index >= 128 or linked(path): return False
            if path.name in {OWNER, '.lock'}:
                if not path.is_file() or path.stat().st_nlink != 1 or path.stat().st_size > 4096: return False
            elif path.name in {'downloads', 'ffmpeg', 'pspdecrypt'}:
                if not path.is_dir(): return False
            elif interrupted_stage(path):
                stages += 1
                if stages > 64: return False
            else: return False
        return True
    except (OSError, ValueError):
        return False


def own_cache(data_root, verify_only):
    root = Path(data_root).expanduser().absolute()
    if linked(root) or root.exists() and not root.is_dir(): fail('unsafe_tool_cache', 'Selected game data root is linked or not a directory')
    for ancestor in root.parents:
        if linked(ancestor): fail('unsafe_tool_cache', 'Selected data root has a linked parent')
    tools = root / 'tools'
    if tools.exists() or linked(tools):
        if not owns_dependency_directory(root): fail('unowned_tool_cache', 'Preserving the existing unowned tools directory in the selected game-data folder')
    else:
        if verify_only: fail('dependencies_not_ready', 'Prepare game once to download the verified local dependencies')
        root.mkdir(parents=True, exist_ok=True); tools.mkdir()
        (tools / OWNER).write_text(json.dumps({'producer': PRODUCER, 'schema': 1}), encoding='utf-8')
    return tools


@contextmanager
def held_cache_file(path):
    """Kernel-owned byte lock: process death releases it without unlink races."""
    path = Path(path)
    if linked(path) or not path.is_file() or path.stat().st_nlink != 1 or path.stat().st_size > 4096:
        fail('unsafe_tool_cache', 'Dependency coordination file is missing, linked, nonregular or oversized')
    try:
        handle = path.open('rb')
    except OSError as error:
        fail('dependency_cache_busy', 'Cannot open dependency coordination file: ' + str(error))
    try:
        try:
            if os.name == 'nt':
                import msvcrt
                # Windows checks requested read ranges even beyond EOF. Keep the
                # byte beyond both <=4KiB metadata and ordinary hashing buffers.
                # Seeking/locking here does not grow or modify the file.
                handle.seek(CACHE_LOCK_BYTE)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            fail('dependency_cache_busy', 'Another setup is preparing or verifying the dependency cache')
        yield
    finally: handle.close()


@contextmanager
def cache_lock(folder, verify_only):
    # The immutable owner marker already exists even in older caches. Locking it
    # coordinates readers/writers without creating or changing ANY metadata in
    # verify-only mode. Legacy PID-only .lock bytes are preserved, not treated as
    # proof of a live owner, and additionally locked if present.
    folder = Path(folder)
    with held_cache_file(folder / OWNER):
        legacy = folder / '.lock'
        if legacy.exists() or linked(legacy):
            with held_cache_file(legacy): yield
        else: yield


def publish_file(temporary, destination):
    """Atomically publish a verified sibling file without replacing any name."""
    temporary, destination = Path(temporary), Path(destination)
    if temporary.parent != destination.parent:
        fail('unsafe_dependency_cache', 'Dependency publication must stay in one directory')
    if os.name == 'nt':
        import ctypes
        move = ctypes.WinDLL('kernel32', use_last_error=True).MoveFileW
        move.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]; move.restype = ctypes.c_int
        if not move(str(temporary), str(destination)): raise ctypes.WinError(ctypes.get_last_error())
    else:
        # link() fails if destination exists; plain rename() would replace it.
        os.link(temporary, destination)
        temporary.unlink()


def obtain_file(destination, record, emit, seed=None, verify_only=False):
    destination = Path(destination)
    if destination.exists() or linked(destination): return verify_file(destination, record, destination.name)
    if verify_only: fail('dependencies_not_ready', 'Missing verified dependency download: ' + destination.name)
    if seed is not None: verify_file(seed, record, 'selected local dependency archive')
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + '.part-' + uuid.uuid4().hex)
    try:
        emit('Preparing dependency: ' + destination.name)
        if seed is not None:
            with Path(seed).open('rb') as incoming, temporary.open('xb') as outgoing:
                shutil.copyfileobj(incoming, outgoing)
                outgoing.flush(); os.fsync(outgoing.fileno())
        else:
            if not record['url'].startswith('https://'): fail('invalid_dependency_configuration', 'Dependency download must use HTTPS')
            request = urllib.request.Request(record['url'], headers={'User-Agent': 'PSPRecomp-local-setup', 'Accept': 'application/octet-stream'})
            with urllib.request.urlopen(request, timeout=60) as incoming, temporary.open('xb') as outgoing:
                done, next_percent = 0, 0
                while block := incoming.read(min(1024 * 1024, record['bytes'] + 1 - done)):
                    done += len(block)
                    if done > record['bytes']: fail('dependency_download_mismatch', 'Dependency download exceeds the pinned size')
                    outgoing.write(block)
                    percent = done * 100 // max(record['bytes'], 1)
                    if percent >= next_percent: emit(f'Downloading {destination.name}: {percent}%'); next_percent = percent // 10 * 10 + 10
                outgoing.flush(); os.fsync(outgoing.fileno())
        verify_file(temporary, record, destination.name)
        # Existing downloads are never overwritten, including a racing setup.
        publish_file(temporary, destination)
        verify_file(destination, record, destination.name)
        return destination
    except (OSError, ValueError) as error:
        fail('dependency_download_failed', f'Cannot obtain {destination.name}: {error}')
    finally:
        temporary.unlink(missing_ok=True)


def extract_zip(archive, destination, max_files=5000, max_bytes=1024 * 1024 * 1024):
    names = set()
    with zipfile.ZipFile(archive) as incoming:
        rows = incoming.infolist()
        if len(rows) > max_files or sum(row.file_size for row in rows) > max_bytes:
            fail('unsafe_dependency_archive', 'Dependency extraction exceeds its bounds')
        for row in rows:
            path = member_name(row.filename.rstrip('/')); key = str(path).casefold()
            if key in names or stat.S_ISLNK(row.external_attr >> 16): fail('unsafe_dependency_archive', 'Duplicate or linked dependency ZIP member')
            names.add(key)
            if row.is_dir(): continue
            target = destination.joinpath(*path.parts); target.parent.mkdir(parents=True, exist_ok=True)
            with incoming.open(row) as source, target.open('xb') as output: shutil.copyfileobj(source, output)


def verify_ffmpeg(prefix, config, sdk):
    prefix = Path(prefix)
    if linked(prefix): fail('unsafe_dependency_cache', 'FFmpeg prefix is linked')
    prefix = prefix.resolve()
    for name, record in config['ffmpeg_runtime_files'].items():
        path = prefix.joinpath(*member_name(name).parts)
        if not path.resolve().is_relative_to(prefix): fail('unsafe_dependency_cache', 'External FFmpeg dependency path')
        verify_file(path, record, name)
    for row in sdk['files']:
        path = prefix.joinpath(*member_name(row['path']).parts)
        if not path.resolve().is_relative_to(prefix): fail('unsafe_dependency_cache', 'External FFmpeg SDK path')
        verify_file(path, row, row['path'])
    if config.get('ffmpeg_files'):
        verify_exact_files(prefix, config['ffmpeg_files'])
    return prefix


def verify_exact_files(folder, expected, extra_files=()):
    """Exact bounded inventory, including empty-directory/link rejection."""
    folder = Path(folder)
    if linked(folder) or not folder.is_dir(): fail('dependency_cache_changed', 'Missing or linked dependency version directory')
    expected_directories = set()
    for name in expected:
        path = member_name(name)
        expected_directories.update(str(parent) for parent in path.parents if str(parent) != '.')
    files, directories = set(), set()
    for path in folder.rglob('*'):
        if linked(path): fail('dependency_cache_changed', 'Linked path in a dependency version')
        relative = path.relative_to(folder).as_posix()
        if path.is_dir(): directories.add(relative)
        elif path.is_file(): files.add(relative)
        else: fail('dependency_cache_changed', 'Nonregular dependency version member')
        if len(files) + len(directories) > 10000: fail('dependency_cache_changed', 'Dependency version inventory exceeds its bound')
    if files != set(expected) | set(extra_files) or directories != expected_directories:
        fail('dependency_cache_changed', 'Preserving dependency version with missing, extra or unknown files/directories')
    for name, record in expected.items(): verify_file(folder.joinpath(*member_name(name).parts), record, name)


def version_file_inventory(config):
    identity = config['ffmpeg']; prefix = str(member_name(identity['prefix']))
    if '/' in prefix or not re.fullmatch('[0-9a-f]{64}', identity['sha256']):
        fail('invalid_dependency_configuration', 'Invalid content-addressed codec identity')
    result = {prefix + '/' + name: record for name, record in config['ffmpeg_files'].items()}
    result['COPYING.GPLv3'] = {key: config['gpl_license'][key] for key in ('bytes', 'sha256')}
    provenance = json.dumps(identity, indent=2).encode('utf-8')
    result['provenance.json'] = {'bytes': len(provenance), 'sha256': hashlib.sha256(provenance).hexdigest()}
    return result


def version_receipt(config):
    identity = config['ffmpeg']
    return {'producer': PRODUCER, 'schema': 1, 'archive_sha256': identity['sha256'],
            'source_commit': identity['source_commit'], 'prefix': identity['prefix'],
            'files': version_file_inventory(config)}


def verify_version_leaf(folder, config):
    receipt = Path(folder) / FFMPEG_RECEIPT
    if linked(receipt) or not receipt.is_file() or receipt.stat().st_size > 1024 * 1024:
        fail('unowned_dependency_version', 'Preserving a dependency version without its exact ownership receipt')
    try: record = json.loads(receipt.read_text(encoding='utf-8'))
    except (OSError, ValueError): fail('unowned_dependency_version', 'Dependency version receipt is unreadable or invalid')
    if record != version_receipt(config): fail('unowned_dependency_version', 'Dependency version receipt does not match the pinned archive/source/files')
    verify_exact_files(folder, record['files'], (FFMPEG_RECEIPT,))
    return Path(folder) / config['ffmpeg']['prefix']


def verify_legacy_ffmpeg(folder, legacy, config):
    """Recognize only the explicitly pinned v3 layout; never modify/adopt it."""
    identity = legacy['identity']; provenance = Path(folder) / 'provenance.json'
    if linked(provenance) or not provenance.is_file() or provenance.stat().st_size > 65536:
        fail('unowned_dependency_version', 'Legacy codec has no verified identity receipt')
    try: record = json.loads(provenance.read_text(encoding='utf-8'))
    except (OSError, ValueError): fail('unowned_dependency_version', 'Legacy codec identity receipt is invalid')
    if record != identity: fail('unowned_dependency_version', 'Legacy codec identity is not an explicitly supported previous version')
    prefix = Path(folder) / str(member_name(identity['prefix']))
    if '/' in identity['prefix']: fail('invalid_dependency_configuration', 'Legacy codec prefix must be one directory')
    verify_exact_files(prefix, legacy['files'])
    verify_file(Path(folder) / 'COPYING.GPLv3', config['gpl_license'], 'legacy codec GPL license')
    return prefix


def validate_ffmpeg_container(tools, config):
    folder = tools / 'ffmpeg'
    if not folder.exists(): return
    if linked(folder) or not folder.is_dir(): fail('unowned_dependency_version', 'Codec container is linked or not a directory')
    legacy = config.get('legacy_ffmpeg')
    legacy_names = {legacy['identity']['prefix'], 'provenance.json', 'COPYING.GPLv3'} if legacy else set()
    actual = {path.name for path in folder.iterdir()}
    if actual & legacy_names:
        if not legacy_names.issubset(actual): fail('dependency_cache_changed', 'Incomplete legacy codec layout is preserved')
        verify_legacy_ffmpeg(folder, legacy, config)
    for child in folder.iterdir():
        if child.name in legacy_names: continue
        if child.name != config['ffmpeg']['sha256']:
            fail('unowned_dependency_version', 'Preserving an unknown codec version or foreign container file')
        verify_version_leaf(child, config)


def pe_sections(path):
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:2] != b'MZ': fail('invalid_decryptor', 'Decryptor is not a Windows PE executable')
    offset = struct.unpack_from('<I', data, 60)[0]
    if offset + 24 > len(data) or data[offset:offset+4] != b'PE\0\0': fail('invalid_decryptor', 'Invalid PE header')
    count = struct.unpack_from('<H', data, offset+6)[0]; optional = struct.unpack_from('<H', data, offset+20)[0]
    if not 1 <= count <= 96: fail('invalid_decryptor', 'Invalid PE section count')
    table = offset + 24 + optional; result = {}
    for index in range(count):
        position = table + index * 40
        if position + 40 > len(data): fail('invalid_decryptor', 'Truncated PE section table')
        name = data[position:position+8].rstrip(b'\0'); size, start = struct.unpack_from('<II', data, position+16)
        if start + size > len(data) or name in result: fail('invalid_decryptor', 'Invalid PE section extent')
        result[name] = hashlib.sha256(data[start:start+size]).hexdigest()
    return result


def sdk_manifest_tool():
    kits = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits/10/bin'
    candidates = [path for path in kits.glob('*/x64/mt.exe') if re.fullmatch(r'\d+(\.\d+)+', path.parent.parent.name)]
    if not candidates: fail('missing_manifest_tool', 'Install the Windows SDK with Visual Studio C++ Build Tools; mt.exe is required for Unicode paths')
    return max(candidates, key=lambda path: tuple(map(int, path.parent.parent.name.split('.'))))


def run_tool(arguments):
    try:
        result = subprocess.run([str(item) for item in arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, encoding='utf-8', errors='replace', timeout=60,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    except (OSError, subprocess.SubprocessError) as error: fail('dependency_tool_failed', 'Cannot run dependency preparation tool: ' + str(error))
    if result.returncode: fail('dependency_tool_failed', 'Dependency preparation tool failed: ' + result.stdout[-4000:])
    return result.stdout


def verify_utf8_resource(path, mt, folder):
    resource = folder / ('manifest-proof-' + uuid.uuid4().hex + '.xml')
    try:
        run_tool([mt, '-nologo', '-inputresource:' + str(path) + ';#1', '-out:' + str(resource)])
        tree = ET.fromstring(resource.read_bytes())
        pages = [node.text for node in tree.iter() if node.tag.endswith('}activeCodePage')]
        privileges = [node.attrib for node in tree.iter() if node.tag.endswith('}requestedExecutionLevel')]
        if pages != ['UTF-8'] or privileges != [{'level': 'asInvoker', 'uiAccess': 'false'}]:
            fail('invalid_decryptor_manifest', 'Local decryptor resource does not declare the verified UTF-8/asInvoker settings')
    finally: resource.unlink(missing_ok=True)


def patch_decryptor(original, output, manifest, mt, config):
    verify_file(original, {'bytes': config['pspdecrypt']['executable_bytes'], 'sha256': config['pspdecrypt']['executable_sha256']}, 'original decryptor')
    if sha(manifest) != config['utf8_manifest_sha256']: fail('invalid_decryptor_manifest', 'UTF-8 resource source changed')
    if output.exists(): fail('dependency_cache_changed', 'Preserving an existing decryptor working copy')
    shutil.copyfile(original, output)
    run_tool([mt, '-nologo', '-manifest', manifest, '-outputresource:' + str(output) + ';#1'])
    before, after = pe_sections(original), pe_sections(output)
    if {name: value for name, value in before.items() if name != b'.rsrc'} != {name: value for name, value in after.items() if name != b'.rsrc'}:
        fail('decryptor_code_changed', 'Manifest preparation changed a non-resource executable section')
    verify_utf8_resource(output, mt, output.parent)
    return sha(output)


def installed_sevenzip(path=None):
    candidate = Path(path) if path is not None else Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / '7-Zip/7z.exe'
    if linked(candidate) or not candidate.is_file(): fail('missing_7zip', 'Install 7-Zip for Windows x64 from https://www.7-zip.org/ before preparing the ISO')
    output = run_tool([candidate, 'i'])
    if '7-Zip' not in output or 'Formats:' not in output: fail('invalid_7zip', 'Selected extraction tool did not identify as 7-Zip')
    return candidate.resolve()


def ensure_dependencies(data_root, emit=print, verify_only=False, sevenzip=None, decryptor=None,
                        ffmpeg_prefix=None, *, ffmpeg_archive=None, decryptor_archive=None, license_file=None):
    """Return verified sevenzip/decryptor/ffmpeg_prefix paths; verification never writes."""
    config, sdk = load_configuration()
    sevenzip = installed_sevenzip(sevenzip)
    if ffmpeg_prefix is not None: ffmpeg_prefix = verify_ffmpeg(ffmpeg_prefix, config, sdk)
    if decryptor is not None:
        decryptor = Path(decryptor)
        if linked(decryptor) or not decryptor.is_file() or sha(decryptor) != config['pspdecrypt']['known_utf8_sha256']:
            fail('dependency_identity_mismatch', 'Explicit development decryptor must match the verified UTF-8 working copy')
    # Verified explicit development tools do not create a cache or download.
    if decryptor is not None and ffmpeg_prefix is not None:
        return {'sevenzip': sevenzip, 'decryptor': decryptor.resolve(), 'ffmpeg_prefix': ffmpeg_prefix}
    tools = own_cache(data_root, verify_only)
    with cache_lock(tools, verify_only):
        validate_ffmpeg_container(tools, config)
        downloads = tools / 'downloads'
        license_path = obtain_file(downloads / 'GPL-3.0.txt', config['gpl_license'], emit, license_file, verify_only)
        if ffmpeg_prefix is None:
            checksum = config['ffmpeg']['sha256']
            archive = obtain_file(downloads / ('ffmpeg-' + checksum + '.zip'), config['ffmpeg'], emit, ffmpeg_archive, verify_only)
            destination = tools / 'ffmpeg' / checksum
            if not destination.exists():
                if verify_only: fail('dependencies_not_ready', 'FFmpeg local extraction is missing')
                destination.parent.mkdir(exist_ok=True)
                with tempfile.TemporaryDirectory(prefix='.ffmpeg-', dir=tools) as temporary:
                    stage = Path(temporary); extract_zip(archive, stage); verify_ffmpeg(stage / config['ffmpeg']['prefix'], config, sdk)
                    shutil.copyfile(license_path, stage / 'COPYING.GPLv3')
                    (stage / 'provenance.json').write_bytes(json.dumps(config['ffmpeg'], indent=2).encode('utf-8'))
                    (stage / FFMPEG_RECEIPT).write_bytes(json.dumps(version_receipt(config), indent=2).encode('utf-8'))
                    verify_version_leaf(stage, config)
                    stage.rename(destination)
            ffmpeg_prefix = verify_version_leaf(destination, config)
        if decryptor is None:
            archive = obtain_file(downloads / 'pspdecrypt.zip', config['pspdecrypt'], emit, decryptor_archive, verify_only)
            destination = tools / 'pspdecrypt'; output = destination / 'pspdecrypt-utf8.exe'
            if not destination.exists():
                if verify_only: fail('dependencies_not_ready', 'Unicode decryptor preparation is missing')
                mt = sdk_manifest_tool()
                manifest = BASE / ('host/windows.manifest' if (BASE / 'host/windows.manifest').is_file() else 'tools/decryptor-utf8.manifest')
                with tempfile.TemporaryDirectory(prefix='.pspdecrypt-', dir=tools) as temporary:
                    stage = Path(temporary); extract_zip(archive, stage, max_files=20, max_bytes=16 * 1024 * 1024)
                    checksum = patch_decryptor(stage / 'pspdecrypt.exe', stage / 'pspdecrypt-utf8.exe', manifest, mt, config)
                    shutil.copyfile(license_path, stage / 'LICENSE.TXT')
                    record = {'producer': PRODUCER, 'original_sha256': config['pspdecrypt']['executable_sha256'], 'working_sha256': checksum,
                        'manifest_sha256': config['utf8_manifest_sha256'], 'mt_sha256': sha(mt), 'source': config['pspdecrypt'],
                        'modification': 'Only Windows UTF-8/asInvoker manifest resource changed; all other raw PE section hashes verified equal.'}
                    (stage / 'provenance.json').write_text(json.dumps(record, indent=2), encoding='utf-8'); stage.rename(destination)
            original = verify_file(destination / 'pspdecrypt.exe', {'bytes': config['pspdecrypt']['executable_bytes'], 'sha256': config['pspdecrypt']['executable_sha256']}, 'original decryptor')
            record = json.loads((destination / 'provenance.json').read_text(encoding='utf-8'))
            if (record.get('producer') != PRODUCER or record.get('original_sha256') != sha(original) or
                record.get('manifest_sha256') != config['utf8_manifest_sha256'] or linked(output) or not output.is_file() or sha(output) != record.get('working_sha256')):
                fail('dependency_identity_mismatch', 'Cached decryptor provenance/working copy changed')
            before, after = pe_sections(original), pe_sections(output)
            if {k:v for k,v in before.items() if k != b'.rsrc'} != {k:v for k,v in after.items() if k != b'.rsrc'}:
                fail('decryptor_code_changed', 'Cached working decryptor code changed')
            verify_file(destination / 'LICENSE.TXT', config['gpl_license'], 'decryptor GPL license')
            decryptor = output
    return {'sevenzip': sevenzip, 'decryptor': decryptor.resolve(), 'ffmpeg_prefix': ffmpeg_prefix}
