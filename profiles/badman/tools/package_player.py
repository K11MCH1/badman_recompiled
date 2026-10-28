"""Assemble the source-only launcher/recompiler; game translations build locally.

Never package a precompiled game executable, generated game code or game inputs.
An existing destination is always preserved, including historical developer folders.
"""
import argparse
import ctypes
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import tempfile
import urllib.request
import zipfile

PROFILE = Path(__file__).resolve().parents[1]
ROOT = PROFILE.parents[1]
PYTHON_URL = 'https://www.python.org/ftp/python/3.13.12/python-3.13.12-embed-amd64.zip'
PYTHON_SHA256 = '76f238f606250c87c6beac75dccd35ee99070a13490555936abb6cb64ecce3d0'
PRODUCER = 'PSPRecomp Badman source-only launcher and recompiler'
VULKAN_HEADERS_COMMIT = 'b379292b2ab6df5771ba9870d53cf8b2c9295daf'
VULKAN_HEADERS_ROOT = 'third_party/vulkan_headers/'
VULKAN_HEADERS_FILES = frozenset({
    'PROVENANCE.json', 'LICENSE.md', 'LICENSES/Apache-2.0.txt', 'LICENSES/MIT.txt',
    'include/vulkan/vulkan_core.h', 'include/vulkan/vk_platform.h',
    'include/vk_video/vulkan_video_codecs_common.h',
    'include/vk_video/vulkan_video_codec_av1std.h', 'include/vk_video/vulkan_video_codec_av1std_decode.h',
    'include/vk_video/vulkan_video_codec_h264std.h', 'include/vk_video/vulkan_video_codec_h264std_decode.h',
    'include/vk_video/vulkan_video_codec_h264std_encode.h',
    'include/vk_video/vulkan_video_codec_h265std.h', 'include/vk_video/vulkan_video_codec_h265std_decode.h',
    'include/vk_video/vulkan_video_codec_h265std_encode.h',
})
VULKAN_SOURCE_PATHS = frozenset(VULKAN_HEADERS_ROOT + n for n in VULKAN_HEADERS_FILES)
VULKAN_PACKAGE_PATHS = frozenset('runtime-source/' + n for n in VULKAN_SOURCE_PATHS)
FFMPEG_PARENT = ROOT / 'out/tools/ffmpeg-8.1-lgpl-edc7450333aa'
FFMPEG_DIRECTORY = FFMPEG_PARENT / 'ffmpeg-n8.1-latest-win64-lgpl-shared-8.1'
FFMPEG_IDENTITY = {
    'version': 'n8.1.3-24-g92611900bb-20261024', 'license': 'LGPL-3.0-or-later',
    'source_commit': '92611900bb112123e9bf997fdcc2d78e4c4922fc',
    'archive_sha256': 'edc7450333aae4042e1f4670aa7bcafe4265324ba9f75abf8de49e8b52812389',
    'archive_size': 80986076, 'asset_id': 672324796,
    'asset_api_url': 'https://api.github.com/repos/BtbN/FFmpeg-Builds/releases/assets/672324796',
}
FFMPEG_VENDOR_FILES = {
    'bin/avcodec-62.dll': (90947584, '264be085d9ef6d09087981beba3be6d23153c4753a5415eccfabc11a7e1090a0'),
    'bin/avutil-60.dll': (3014144, '3ba430598f7020539c9fa4711bce6785eb760262e590083b3e77434b982f5bbe'),
    # avcodec imports swresample even for video-only use; do not omit it.
    'bin/swresample-6.dll': (734720, 'feaef02141f2fa5ef04904cf2c89f6accd9b2236eeb7805ca57a5df2480850a9'),
    'bin/swscale-9.dll': (12900864, 'a2b0785591a69e10d23c025a2e18afe6ee42c778b90ee2bee07d2ac353b7bc8c'),
    'LICENSE.txt': (7651, 'da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768'),
}
FFMPEG_PROVENANCE_FILES = {
    'COPYING.GPLv3': (35147, '8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903'),
    'build-info.txt': (2464, '7382198256f5fef3362d99012a4675ade4b78832c905b1aa88a05fba4f7cbc6f'),
    'license-report.txt': (627, '88550395686e6ee3637530ceae4d9053adc0895c78530def72c2e4966228b3d3'),
    'provenance.json': (2118, 'f50ed2d77dcf60d4dd8b2ab46be1736c919f2807ed5b5d53888ebc26ef6b7a7f'),
}


class PackageError(ValueError):
    pass


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as handle:
        while chunk := handle.read(1024 * 1024):
            result.update(chunk)
    return result.hexdigest()


def linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def relative_name(name):
    path = PurePosixPath(name)
    reserved = {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)), *(f'LPT{i}' for i in range(1, 10))}
    if (not name or not path.parts or '\\' in name or ':' in name or path.is_absolute() or
            any(part in ('', '.', '..') or part.endswith((' ', '.')) or
                part.split('.')[0].upper() in reserved or any(ord(char) < 32 for char in part) for part in path.parts)):
        raise PackageError('Unsafe package-relative path: ' + name)
    return path


@dataclass(frozen=True)
class Entry:
    source: Path
    relative: str
    component: str
    expected_sha256: str | None = None
    expected_bytes: int | None = None


def runtime_archive(path):
    path = Path(path)
    if path.exists():
        if digest(path) != PYTHON_SHA256:
            raise PackageError('Preserving an existing Python archive with an unexpected hash')
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(PYTHON_URL, timeout=60) as response:
        data = response.read(20 * 1024 * 1024 + 1)
    if len(data) > 20 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != PYTHON_SHA256:
        raise PackageError('Official Python runtime download failed its published SHA-256 check')
    with path.open('xb') as handle:
        handle.write(data)
    return path


def extract_python(archive, destination, expected_hash=PYTHON_SHA256):
    if digest(archive) != expected_hash:
        raise PackageError('Python runtime archive identity mismatch')
    names = set()
    with zipfile.ZipFile(archive) as source:
        if len(source.infolist()) > 1000 or sum(row.file_size for row in source.infolist()) > 100 * 1024 * 1024:
            raise PackageError('Python runtime archive exceeds bounded extraction limits')
        for row in source.infolist():
            path = relative_name(row.filename.rstrip('/'))
            name = str(path).casefold()
            if name in names or stat.S_ISLNK(row.external_attr >> 16):
                raise PackageError('Duplicate or linked Python archive member')
            names.add(name)
            if row.is_dir():
                continue
            target = destination.joinpath(*path.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(row) as incoming, target.open('xb') as outgoing:
                shutil.copyfileobj(incoming, outgoing)
    required = {'python.exe', 'python313.dll', 'python313.zip', 'python313._pth', 'license.txt'}
    if not required.issubset(names):
        raise PackageError('Python archive is missing the expected embeddable runtime/license')
    (destination / 'python313._pth').write_text('python313.zip\n.\n../tools\n', encoding='utf-8')


def verify_pe(path, subsystem, require_utf8=True):
    data = path.read_bytes()
    if len(data) < 128 or data[:2] != b'MZ':
        raise PackageError('Expected a built Windows executable: ' + str(path))
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if (pe + 96 > len(data) or data[pe:pe + 4] != b'PE\0\0' or
            struct.unpack_from('<H', data, pe + 4)[0] != 0x8664 or
            struct.unpack_from('<H', data, pe + 24)[0] != 0x20b or
            struct.unpack_from('<H', data, pe + 24 + 68)[0] != subsystem):
        raise PackageError('Expected the correct AMD64 Windows executable type: ' + str(path))
    if require_utf8 and (b'activeCodePage' not in data or b'UTF-8' not in data):
        raise PackageError('Executable lacks the verified UTF-8 manifest; rebuild it: ' + str(path))


def verify_dll(path):
    """Bounded header inspection; an EXE or a wrong-architecture DLL cannot substitute."""
    with Path(path).open('rb') as handle:
        header = handle.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise PackageError('Expected an AMD64 codec DLL: ' + str(path))
        offset = struct.unpack_from('<I', header, 0x3c)[0]
        if offset < 64 or offset > 1024 * 1024 or offset + 26 > Path(path).stat().st_size:
            raise PackageError('Invalid codec DLL PE header extent')
        handle.seek(offset); pe = handle.read(26)
        if (pe[:4] != b'PE\0\0' or struct.unpack_from('<H', pe, 4)[0] != 0x8664 or
                not struct.unpack_from('<H', pe, 22)[0] & 0x2000 or struct.unpack_from('<H', pe, 24)[0] != 0x20b):
            raise PackageError('Expected the pinned AMD64 shared codec DLL type: ' + str(path))


def ffmpeg_entries(directory, archive, provenance_directory, codec_config=None, *,
                   identity=None, vendor_pins=None, provenance_pins=None, sdk_manifest=None):
    """Verify the exact external dependency; optional pins are test injection only."""
    identity = FFMPEG_IDENTITY if identity is None else identity
    vendor_pins = FFMPEG_VENDOR_FILES if vendor_pins is None else vendor_pins
    provenance_pins = FFMPEG_PROVENANCE_FILES if provenance_pins is None else provenance_pins
    directory, archive, provenance_directory = Path(directory), Path(archive), Path(provenance_directory)
    codec_config = Path(codec_config) if codec_config is not None else PROFILE / 'config/video_codec.json'
    for folder in (directory, directory / 'bin', provenance_directory):
        if linked(folder) or not folder.is_dir():
            raise PackageError('Missing/linked FFmpeg dependency directory: ' + str(folder))
    if (linked(archive) or not archive.is_file() or archive.stat().st_size != identity['archive_size'] or
            digest(archive) != identity['archive_sha256']):
        raise PackageError('Pinned published FFmpeg archive identity mismatch')
    entries = []
    for folder, pins in ((directory, vendor_pins), (provenance_directory, provenance_pins)):
        for name, (size, checksum) in pins.items():
            relative_name(name)
            source = folder.joinpath(*PurePosixPath(name).parts)
            if linked(source) or not source.is_file() or source.stat().st_size != size or digest(source) != checksum:
                raise PackageError('Pinned FFmpeg dependency input mismatch: ' + name)
            if source.suffix.casefold() == '.dll': verify_dll(source)
            entries.append(Entry(source, 'tools/ffmpeg/' + name, 'external-FFmpeg-LGPLv3', checksum, size))
    provenance = json.loads((provenance_directory / 'provenance.json').read_text(encoding='utf-8-sig'))
    for key, expected in identity.items():
        if provenance.get(key) != expected:
            raise PackageError('FFmpeg source/build provenance identity mismatch: ' + key)
    listed = {row.get('path'): row for row in provenance.get('files', [])}
    for name, (size, checksum) in vendor_pins.items():
        if listed.get(name, {}).get('bytes') != size or listed.get(name, {}).get('sha256') != checksum:
            raise PackageError('FFmpeg provenance file inventory mismatch: ' + name)
    if linked(codec_config) or not codec_config.is_file() or codec_config.stat().st_size > 65536:
        raise PackageError('Missing/linked codec identity configuration')
    configuration = json.loads(codec_config.read_text(encoding='utf-8-sig'))
    for key, expected in identity.items():
        if configuration.get(key) != expected:
            raise PackageError('Codec configuration does not match the pinned dependency: ' + key)
    expected_dlls = {PurePosixPath(name).name for name in vendor_pins if name.endswith('.dll')}
    if (len(configuration.get('required_runtime_dlls', [])) != len(expected_dlls) or
            set(configuration.get('required_runtime_dlls', [])) != expected_dlls):
        raise PackageError('Codec configuration must declare exactly the required runtime DLLs')
    entries.append(Entry(codec_config, 'config/video_codec.json', 'codec-identity-and-provenance',
                         digest(codec_config), codec_config.stat().st_size))
    if sdk_manifest is not None:
        sdk_manifest = Path(sdk_manifest)
        record = json.loads(sdk_manifest.read_text(encoding='utf-8'))
        if record.get('archive_sha256') != identity['archive_sha256'] or record.get('source_commit') != identity['source_commit']:
            raise PackageError('FFmpeg SDK manifest does not match the pinned runtime')
        seen = set()
        for row in record.get('files', []):
            name = row.get('path', ''); path = relative_name(name)
            if len(path.parts) != 3 or path.parts[0] != 'include' or path.suffix != '.h' or name.casefold() in seen:
                raise PackageError('Invalid/duplicate public FFmpeg SDK header path')
            seen.add(name.casefold()); source = directory.joinpath(*path.parts)
            if (linked(source) or not source.is_file() or not source.resolve().is_relative_to(directory.resolve()) or
                    source.stat().st_size != row['bytes'] or digest(source) != row['sha256']):
                raise PackageError('Pinned FFmpeg SDK header mismatch: ' + name)
            entries.append(Entry(source, 'tools/ffmpeg/' + name, 'external-FFmpeg-public-SDK', row['sha256'], row['bytes']))
        required = {'include/libavcodec/avcodec.h', 'include/libavutil/avutil.h', 'include/libswscale/swscale.h', 'include/libswresample/swresample.h'}
        if not required.issubset(seen):
            raise PackageError('Matching public codec SDK headers are incomplete')
        entries.append(Entry(sdk_manifest, 'tools/ffmpeg/sdk-manifest.json', 'codec-public-SDK-provenance', digest(sdk_manifest), sdk_manifest.stat().st_size))
    return entries


def vulkan_headers_provenance(directory):
    """Validate only the reviewed official header closure, never arbitrary vendor files."""
    directory = Path(directory)
    if linked(directory) or not directory.is_dir():
        raise PackageError('Missing/linked Vulkan-Headers source directory')
    if any(linked(p) for p in directory.rglob('*')):
        raise PackageError('Vulkan-Headers directory contains a link')
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    if actual != VULKAN_HEADERS_FILES:
        raise PackageError('Vulkan-Headers must contain exactly the reviewed headers, provenance and licenses')
    for name in VULKAN_HEADERS_FILES:
        file = directory / name
        if (not file.is_file() or linked(file) or not file.resolve().is_relative_to(directory.resolve()) or
                any(linked(p) for p in file.parents if p.is_relative_to(directory))):
            raise PackageError('Linked/external Vulkan-Headers input: ' + name)
    provenance = directory / 'PROVENANCE.json'
    if provenance.stat().st_size > 65536:
        raise PackageError('Oversized Vulkan-Headers provenance')
    record = json.loads(provenance.read_text(encoding='utf-8'))
    if (not isinstance(record, dict) or record.get('project') != 'KhronosGroup/Vulkan-Headers' or
            record.get('commit') != VULKAN_HEADERS_COMMIT):
        raise PackageError('Vulkan-Headers source identity is not the reviewed Khronos commit')
    rows = record.get('files', [])
    if not isinstance(rows, list) or not all(isinstance(r, dict) and isinstance(r.get('path'), str) for r in rows):
        raise PackageError('Malformed Vulkan-Headers provenance file inventory')
    names = [r.get('path') for r in rows]
    if len(names) != len(set(names)) or set(names) != VULKAN_HEADERS_FILES - {'PROVENANCE.json'}:
        raise PackageError('Vulkan-Headers provenance inventory is incomplete or unapproved')
    for row in rows:
        file = directory / row['path']
        expected_url = 'https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/' + record['commit'] + '/' + row['path']
        if row.get('url') != expected_url or row.get('bytes') != file.stat().st_size or row.get('sha256') != digest(file):
            raise PackageError('Pinned Vulkan-Headers input mismatch: ' + row['path'])
    return record


def source_code_entries(root, manifest):
    """Read a reviewed exact inventory; never recursively copy a repository."""
    root, manifest = Path(root).resolve(), Path(manifest)
    record = json.loads(manifest.read_text(encoding='utf-8'))
    if record.get('schema') != 1 or record.get('game_independent_source_only') is not True:
        raise PackageError('Expected reviewed source-only inventory')
    entries, names, vendor_names = [], set(), set()
    for row in record.get('files', []):
        name = row.get('path', ''); path = relative_name(name)
        is_vulkan = name in VULKAN_SOURCE_PATHS
        allowed = (is_vulkan or name in {'CMakeLists.txt', 'LICENSE'} or
            len(path.parts) == 2 and path.parts[0] == 'src' and path.suffix == '.cpp' or
            len(path.parts) == 3 and path.parts[:2] == ('include', 'psprecomp') and path.suffix == '.hpp' or
            name in {'tools/analyzer_main.cpp', 'tools/codegen_main.cpp', 'tools/dump_function.cpp', 'tools/iso_extract_main.cpp',
                     'tools/windows.manifest'} or
            name == 'profiles/badman/CMakeLists.txt' or
            len(path.parts) == 4 and path.parts[:2] == ('profiles', 'badman') and
                (path.parts[2] == 'host' and path.suffix in {'.cpp', '.hpp', '.manifest', '.hlsl', '.rc', '.ico'} or
                 path.parts[2] == 'config' and path.suffix in {'.json', '.csv'} or
                 path.parts[2] == 'tools' and path.name in {'generate.py', 'generate_font.py', 'setup_game.py', 'build_game.py', 'toolchain_bootstrap.py', 'prepare_font.py', 'reconstruct_font.py', 'player_storage.py',
                                                    'embed_profile_facts.py'}) or
            len(path.parts) == 5 and path.parts[:4] == ('profiles', 'badman', 'tools', 'native') and path.suffix == '.cpp')
        if not allowed or name.casefold() in names or path.name.casefold().startswith(('badman_unit_', 'badman_font_unit_', 'font_unit_', 'badman_font_imports.', 'badman_font_registry.')):
            raise PackageError('Unapproved source-only inventory path: ' + name)
        names.add(name.casefold()); source = root.joinpath(*path.parts)
        if is_vulkan: vendor_names.add(name)
        if linked(source) or not source.is_file() or not source.resolve().is_relative_to(root):
            raise PackageError('Missing/linked independent source: ' + name)
        if source.stat().st_size != row['bytes'] or digest(source) != row['sha256']:
            raise PackageError('Reviewed source changed; refresh its reviewed inventory before packaging: ' + name)
        entries.append(Entry(source, 'runtime-source/' + name, 'independent-runtime-source', row['sha256'], row['bytes']))
    if vendor_names:
        if vendor_names != VULKAN_SOURCE_PATHS:
            raise PackageError('Reviewed source inventory omits required Vulkan-Headers licenses or inputs')
        vulkan_headers_provenance(root / VULKAN_HEADERS_ROOT)
    required = {'CMakeLists.txt', 'src/runtime.cpp', 'tools/codegen_main.cpp', 'profiles/badman/CMakeLists.txt',
                'profiles/badman/tools/generate.py', 'profiles/badman/tools/generate_font.py', 'profiles/badman/host/main.cpp'}
    if not {name.casefold() for name in required}.issubset(names): raise PackageError('Reviewed source inventory is incomplete')
    entries.append(Entry(manifest, 'runtime-source/source-manifest.json', 'reviewed-source-inventory', digest(manifest), manifest.stat().st_size))
    return entries


# The launcher window is an independent HTML/CSS/JS page hosted by WebView2.
# Its backgrounds are captured from the player's own game at preparation time;
# no game art is part of the page.
LAUNCHER_UI_FILES = ('index.html', 'launcher.css', 'launcher.js', 'hand.svg')
LAUNCHER_FONT = {  # Pixelify Sans variable TTF, google/fonts ofl/pixelifysans
    'PixelifySans.ttf': (79160, '9ba86cd010a4de309d263ceff8e8044092c9db7efda869620cb9ff1c4389e8a5'),
    'OFL.txt': (4488, 'b66ba46f511a851ab09998b5a5a9fdbb102545a3864cb993095e1745996873a7'),
}
# BadmanLauncher statically links the WebView2 SDK loader (NuGet
# Microsoft.Web.WebView2 1.0.2903.40); its BSD licence and notice ship with it.
WEBVIEW2_SDK = ROOT / 'out/tools/webview2-1.0.2903.40/pkg'
WEBVIEW2_LICENSES = {
    'LICENSE.txt': (1487, '0af8f1b807512aae39c2ac1aa4d0cae65cabecb6fd554b8439a5162a0d6eca55'),
    'NOTICE.txt': (3894, '106423785c5b7eba0a8e61d1837f2132e9c828e20ad530f565d981c1df60dd90'),
}


def source_entries(build, font_directory, font_manifest, *,
                   compiler=None, source_root=ROOT, source_manifest=PROFILE / 'config/source_bundle.json',
                   webview2_sdk=WEBVIEW2_SDK):
    entries = []
    def add(source, relative, component, checksum=None, size=None):
        source = Path(source)
        if linked(source) or not source.is_file():
            raise PackageError('Missing/linked required package input: ' + str(source))
        entries.append(Entry(source, relative, component, checksum, size))
    for name, subsystem in [('BadmanLauncher.exe', 2)]:
        source = Path(build) / name
        if not source.is_file():
            raise PackageError('Build the independent launcher target before packaging: ' + str(source))
        verify_pe(source, subsystem)
        add(source, name, 'independent-launcher')
    for name in LAUNCHER_UI_FILES:
        add(PROFILE / 'launcher-ui' / name, 'launcher-ui/' + name, 'independent-launcher-ui')
    for name, (size, checksum) in LAUNCHER_FONT.items():
        add(PROFILE / 'launcher-ui/fonts' / name, 'launcher-ui/fonts/' + name,
            'open-license-launcher-font', checksum, size)
    for name, (size, checksum) in WEBVIEW2_LICENSES.items():
        add(Path(webview2_sdk) / name, 'licenses/WebView2-SDK-' + name, 'WebView2-SDK-license', checksum, size)
    compiler = Path(compiler) if compiler is not None else ROOT / 'out/badman/Release/psp_recomp.exe'
    verify_pe(compiler, 3)
    add(compiler, 'tools/psp_recomp.exe', 'independent-recompiler')
    entries.extend(source_code_entries(source_root, source_manifest))
    for name in ('setup_game.py', 'build_game.py', 'toolchain_bootstrap.py', 'reconstruct_font.py', 'prepare_font.py', 'player_storage.py',
                 'launcher_art.py'):
        add(PROFILE / 'tools' / name, 'tools/' + name, 'independent-profile-code')
    for name in ('executable.json', 'font_module.json', 'font_relocations.json', 'video_codec.json', 'toolchain_dependencies.json', 'ffmpeg_sdk_files.json'):
        add(PROFILE / 'config' / name, 'config/' + name, 'profile-identity-and-relocation-facts')
    add(ROOT / 'LICENSE', 'licenses/PSPRecomp-MIT.txt', 'project-license')
    add(PROFILE / 'host/windows.manifest', 'tools/decryptor-utf8.manifest', 'independent-tool-resource-modification')
    manifest = json.loads(Path(font_manifest).read_text(encoding='utf-8'))
    files = manifest.get('files', [])
    expected = {'jpn0.pgf', *(f'ltn{i}.pgf' for i in range(8))}
    if len(files) != 9 or {row.get('name') for row in files} != expected:
        raise PackageError('Expected exactly the nine pinned open-license replacement fonts')
    for row in files:
        name = row['name']; relative_name(name)
        source = Path(font_directory) / name
        if not source.is_file() or digest(source) != row.get('sha256') or source.stat().st_size != row.get('size'):
            raise PackageError('Replacement font identity mismatch: ' + name)
        add(source, 'assets/fonts/' + name, 'open-license-replacement-fonts', row['sha256'], row['size'])
    add(font_manifest, 'assets/replacement-font-manifest.json', 'font-provenance')
    add(Path(font_directory).parents[1] / 'UME-FONT-LICENSE.txt', 'assets/UME-FONT-LICENSE.txt', 'font-license')
    return entries


README = '''Badman Recompiled — source-only launcher and local recompiler

Run BadmanLauncher.exe. On the Play page, choose your own supported game ISO and
a game-data folder, then Prepare and build or PLAY. Supported identity: ULUS10456
version 1.01. Preparation verifies and extracts your ISO, generates C++ locally
and compiles your personal native executable inside that selected data folder.
Initial local compilation takes time and disk space. No precompiled game
executable or generated game translation is included in this distribution.
The replacement fonts are selected automatically. After the first successful
preparation the launcher captures your own title screen once (about 30 seconds)
and uses it as the Play background; the other pages use a plain background
unless you place your own pictures in BadmanRecomp/art in your local application
data (display.png, graphics.png, gameplay.png, controls.png, advanced.png,
credits.png). No game artwork is included in this distribution.
The launcher is driven by mouse, keyboard (arrows, Enter, Esc) or an XInput
controller (D-pad/stick, A, B, LB/RB switch pages). Pages: Play, Display
(fullscreen, picture scaling, ultra-wide dungeon view up to 21:9), Graphics (Rendering API, smooth motion, filtering), Gameplay
(skip intro and logos, automatic save profile, FPS counter), Controls
(controller, stick dead zone), Advanced (compiler jobs, verification, folders)
and Credits. Settings are saved and apply the next time you press PLAY.
Rendering API offers Direct3D 11 (default), Vulkan and Software; switching APIs
does not require another game compilation. If your driver does not support the
selected API, choose another one explicitly. The optional yellow top-left FPS
counter counts guest framebuffer submissions, not window repaints. F11 or
Alt+Enter toggles fullscreen while playing.
"Smooth motion up to your refresh rate" (on by default) draws in-between frames
on every display refresh, for example 165 Hz, from the draws behind each game
frame. The game itself keeps its original 30 frames per second and speed; the
FPS counter then shows both, as in "FPS: 165 (game 30.0)". It adds about one game
frame of display delay and can be turned off on the Graphics page.
"Skip intro and logos" skips the boot logo movie through the game's own movie
cancel path. Saves live in a Saves folder inside your game-data folder ("Open
Saves" shows it). One profile is created there silently: the boot Memory Stick
prompt is answered automatically, an existing native save loads without a slot
chooser, and in-game Save writes that profile directly. Previous saves are kept
as recovery history; Delete still asks for confirmation. Saves from older
launcher versions are copied into Saves the first time; originals are kept.
The supplied Python and independent preparation scripts are local to this folder. No global
Python installation or Windows locale change is required. Replacement fonts
are included; the ISO, decrypted game/module executables and game assets are not.

This is an independent launcher/recompiler with independently written runtime
and profile sources. Development runtime tests cleared Training 1 through 3 with
audio and verified their completion after native saving and a fresh-process load.
The first two Story stages have also cleared through ordinary controller input.
Those gameplay checks are separate from this bundle's own-ISO compilation and
startup checks. Later Story stages, tutorials and release validation remain in
progress. This is not a complete playable release; successful setup alone does
not establish that all game features work.

First preparation downloads the exact verified FFmpeg publisher archive (matching
shared libraries and SDK), original pspdecrypt release and its license into the
selected data folder's owned tools cache. Downloads are hash-checked; their
licenses, original files and provenance remain there. No FFmpeg, pspdecrypt or
7-Zip binaries/headers are supplied in this distribution. Internet access is
required for first preparation unless verified archives are supplied explicitly.

Requirements: Windows 10 version 22H2 or newer, x64; the Microsoft Edge WebView2
Runtime (included with Windows 11 and current Windows 10; otherwise
https://developer.microsoft.com/microsoft-edge/webview2/ ); installed 7-Zip from
https://www.7-zip.org/ . Install Visual Studio 2022 Build Tools with Desktop
development with C++, a Windows SDK and C++ CMake tools for Windows. The local
build detects that installed toolchain; Visual Studio tools are not bundled.
Official installer: https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
The launcher/recompiler use the static MSVC runtime. Local game builds also select
the static runtime; bundled Python retains its original publisher runtime files.
This folder has not been tested on a clean Windows installation.

Keyboard (when the native game reaches controller sampling): arrows D-pad;
Z Cross, X Circle, A Square, S Triangle, Enter Start, Backspace Select,
Q/E shoulder buttons. Focus loss releases keys.
XInput controllers (Xbox controllers, Steam Deck; other pads through Steam Input):
A Cross, B Circle, X Square, Y Triangle, Start, Back Select, LB/RB shoulder
buttons, D-pad, left stick analog with the dead zone set on the Controls page.
This controller mapping has not been tested with physical hardware.

Prepared game data stays outside this player folder in your selected directory.
Settings live in your local application data BadmanRecomp/launcher.ini.
See THIRD_PARTY_NOTICES.txt and package-manifest.json for licenses and hashes.
'''
NOTICES_TEMPLATE = '''Third-party components — source-only launcher/recompiler bundle

PSPRecomp independent code: MIT, licenses/PSPRecomp-MIT.txt. This includes the
launcher page in launcher-ui (HTML, CSS, JavaScript and the pointing-hand image).

Microsoft WebView2 SDK 1.0.2903.40 (NuGet Microsoft.Web.WebView2): its loader is
statically linked into BadmanLauncher.exe. BSD-style licence and notices:
licenses/WebView2-SDK-LICENSE.txt and licenses/WebView2-SDK-NOTICE.txt. The
WebView2 Runtime itself is a separately installed Windows component.

Pixelify Sans (launcher-ui/fonts/PixelifySans.ttf), Copyright 2021 The Pixelify
Sans Project Authors, SIL Open Font License 1.1: launcher-ui/fonts/OFL.txt.
Source: https://github.com/google/fonts/tree/main/ofl/pixelifysans
No compiled BadmanNative executable, AOT library, generated game C++ or commercial
input is included. Those are produced locally from the player's verified ISO.

CPython 3.13.12 Windows embeddable x64: Python Software Foundation license and
included third-party notices, python/LICENSE.txt. Original official archive:
https://www.python.org/ftp/python/3.13.12/python-3.13.12-embed-amd64.zip
Only python313._pth is changed to add ../tools, with user/site imports disabled.

7-Zip is a separately installed prerequisite: https://www.7-zip.org/ . No 7-Zip
binary is bundled. First preparation downloads the pinned original pspdecrypt1.0
release from https://github.com/John-K/pspdecrypt/releases/tag/1.0 into the user's
local cache, with GPL license and provenance. An owned working copy receives only
our UTF-8/asInvoker manifest resource using the user's installed Windows SDK tool.
Original files and hashes are retained. tools/decryptor-utf8.manifest is our
independent resource modification. No decryptor binary/source is supplied here.

Replacement PGFs: Ume-derived open-license fonts, assets/UME-FONT-LICENSE.txt.
Pinned URLs/hashes/contributor provenance: assets/replacement-font-manifest.json.
No PPSSPP implementation is linked/copied as part of these font assets.

FFmpeg external shared codec: LGPLv3-or-later, downloaded by first preparation
from the pinned publisher asset into the user's local tools cache. Matching
DLLs/public SDK and vendor LICENSE remain there. No FFmpeg binary, header or
implementation is included in this distribution. Pinned FFmpeg version:
{ffmpeg_version}
Publisher release: {ffmpeg_release}
Exact FFmpeg source commit: {ffmpeg_commit}
{ffmpeg_source_url}
Publisher archive SHA-256: {ffmpeg_archive_sha256}
Published build/archive/file identities: config/toolchain_dependencies.json,
config/video_codec.json and config/ffmpeg_sdk_files.json. Downloaded provenance
and licenses stay beside the locally extracted dependency files.
Build publisher/source scripts: https://github.com/BtbN/FFmpeg-Builds .

Visual Studio 2022 C++ Build Tools, Windows SDK and CMake: installed prerequisites,
not bundled or downloaded by this packager. Our launcher/recompiler and local
game-build command select the static MSVC runtime.
'''


def third_party_notices(config_directory, vulkan_directory=None):
    """Describe the exact dependency configuration shipped beside this notice."""
    directory = Path(config_directory)
    try:
        config = json.loads((directory / 'toolchain_dependencies.json').read_text(encoding='utf-8'))
        codec = json.loads((directory / 'video_codec.json').read_text(encoding='utf-8'))
        ffmpeg = config['ffmpeg']
        identities = {'source_commit': 'source_commit', 'source_url': 'source_url',
                      'sha256': 'archive_sha256', 'bytes': 'archive_size',
                      'url': 'archive_url', 'asset_id': 'asset_id', 'license': 'license'}
        if (config.get('schema') != 1 or
                any(ffmpeg[key] != codec[value] for key, value in identities.items()) or
                ffmpeg['license'] != 'LGPL-3.0-or-later' or
                not re.fullmatch(r'[0-9a-f]{40}', ffmpeg['source_commit']) or
                not re.fullmatch(r'[0-9a-f]{64}', ffmpeg['sha256']) or
                ffmpeg['source_url'] != 'https://github.com/FFmpeg/FFmpeg/tree/' + ffmpeg['source_commit'] or
                any(not isinstance(value, str) or not value or any(ord(c) < 32 for c in value)
                    for value in (codec['version'], ffmpeg['release_tag']))):
            raise ValueError('Dependency and codec identities disagree or are invalid')
        notice = NOTICES_TEMPLATE.format(ffmpeg_version=codec['version'], ffmpeg_release=ffmpeg['release_tag'],
            ffmpeg_commit=ffmpeg['source_commit'], ffmpeg_source_url=ffmpeg['source_url'],
            ffmpeg_archive_sha256=ffmpeg['sha256'])
        if vulkan_directory is not None:
            vk = vulkan_headers_provenance(vulkan_directory)
            notice += ('\nKhronos Vulkan-Headers: vendored API declarations only, SPDX Apache-2.0.\n'
                'The upstream repository also supplies MIT licensing for applicable files.\n'
                'Exact source commit: ' + vk['commit'] + '\n'
                'https://github.com/KhronosGroup/Vulkan-Headers/tree/' + vk['commit'] + '\n'
                'Full upstream license notice and Apache-2.0/MIT texts: runtime-source/third_party/vulkan_headers/LICENSE.md\n'
                'and runtime-source/third_party/vulkan_headers/LICENSES/. Per-file hashes/URLs: PROVENANCE.json there.\n'
                'No Vulkan driver, SDK or DXC shader compiler is bundled. The independent shader is embedded as SPIR-V.\n')
        return notice
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise PackageError('Cannot produce exact dependency notices: ' + str(error)) from error


def assemble(output, entries, archive, allowed_root, archive_hash=PYTHON_SHA256):
    output, allowed_root = Path(output).absolute(), Path(allowed_root).resolve()
    if not output.resolve().is_relative_to(allowed_root) or output.resolve() == allowed_root:
        raise PackageError('Package output must remain inside its explicit ignored output root')
    if output.exists() or linked(output):
        raise PackageError('Preserving existing package output; choose a new destination')
    names = set()
    for entry in entries:
        path = relative_name(entry.relative); key = str(path).casefold()
        if (key in names or key in {'readme.txt', 'third_party_notices.txt', 'package-manifest.json'} or
                path.parts[0].casefold() == 'python' or path.suffix.lower() in {'.iso', '.elf', '.prx', '.bin', '.psar', '.lib', '.obj', '.pdb', '.a', '.o', '.so', '.dylib'} or
                path.name.casefold() == 'badmannative.exe' or any(part.casefold() in {'generated', 'font_generated', 'game', 'analysis', 'progress'} for part in path.parts) or
                path.suffix.casefold() == '.exe' and key not in {'badmanlauncher.exe', 'tools/psp_recomp.exe'} or
                path.suffix.casefold() == '.dll' or
                path.suffix.casefold() == '.h' and entry.relative not in VULKAN_PACKAGE_PATHS or
                any(part.casefold() == 'third_party' for part in path.parts) and entry.relative not in VULKAN_PACKAGE_PATHS or
                any(part.casefold() in {'ffmpeg', 'pspdecrypt', '7zip'} for part in path.parts) or
                path.name.casefold().startswith(('badman_unit_', 'badman_imports.', 'generated_registry.', 'badman_font_unit_', 'badman_font_imports.', 'badman_font_registry.', 'font_unit_', 'font_imports.'))):
            raise PackageError('Duplicate, reserved, or commercial-input package path: ' + entry.relative)
        names.add(key)
        if linked(entry.source) or not entry.source.is_file():
            raise PackageError('Package source must be a regular unlinked file')
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.badman-player-', dir=output.parent)).resolve()
    components = {}
    try:
        for entry in entries:
            target = stage.joinpath(*relative_name(entry.relative).parts); target.parent.mkdir(parents=True, exist_ok=True)
            before = digest(entry.source)
            if ((entry.expected_sha256 is not None and before != entry.expected_sha256) or
                    (entry.expected_bytes is not None and entry.source.stat().st_size != entry.expected_bytes)):
                raise PackageError('Pinned package input changed after preflight: ' + entry.relative)
            shutil.copy2(entry.source, target)
            if digest(target) != before or digest(entry.source) != before:
                raise PackageError('Package input changed while being copied')
            components[entry.relative] = entry.component
        extract_python(archive, stage / 'python', archive_hash)
        (stage / 'README.txt').write_text(README, encoding='utf-8')
        notice_config = stage / 'config'
        if not any((notice_config / name).exists() for name in ('toolchain_dependencies.json', 'video_codec.json')):
            # The generic assembler also supports synthetic inventories in tests;
            # production source_entries always supplies both configuration files.
            notice_config = PROFILE / 'config'
        vulkan_directory = stage / 'runtime-source' / VULKAN_HEADERS_ROOT
        (stage / 'THIRD_PARTY_NOTICES.txt').write_text(third_party_notices(notice_config,
            vulkan_directory if vulkan_directory.exists() else None), encoding='utf-8')
        files = [{'path': file.relative_to(stage).as_posix(), 'bytes': file.stat().st_size, 'sha256': digest(file),
                  'component': components.get(file.relative_to(stage).as_posix(),
                      'python-embeddable' if file.is_relative_to(stage / 'python') else 'package-documentation')}
                 for file in sorted(stage.rglob('*')) if file.is_file()]
        manifest = {'producer': PRODUCER, 'schema': 2, 'source_only_distribution': True,
                    'contains_precompiled_game': False, 'requires_local_game_compilation': True,
                    'python_version': '3.13.12', 'python_archive_url': PYTHON_URL,
                    'python_archive_sha256': archive_hash, 'files': files}
        (stage / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        if output.exists() or linked(output):
            raise PackageError('Package destination appeared during assembly; preserving it')
        if os.name == 'nt':
            # MoveFileW never replaces an existing destination, even an empty dir.
            move = ctypes.WinDLL('kernel32', use_last_error=True).MoveFileW
            move.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]; move.restype = ctypes.c_int
            if not move(str(stage), str(output)):
                raise PackageError('Cannot atomically publish package without replacing another path (Windows error ' +
                    str(ctypes.get_last_error()) + ')')
        else:
            stage.rename(output)
        return manifest
    finally:
        if stage.exists() and not linked(stage) and stage.parent == output.parent.resolve() and stage.name.startswith('.badman-player-'):
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'out/badman-recompiler-source')
    parser.add_argument('--build', type=Path, default=ROOT / 'out/badman/bin/Release')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'out/badman/Release/psp_recomp.exe')
    parser.add_argument('--source-manifest', type=Path, default=PROFILE / 'config/source_bundle.json')
    parser.add_argument('--python-archive', type=Path, default=ROOT / 'out/tools/python-3.13.12-embed-amd64.zip')
    parser.add_argument('--font-directory', type=Path, default=PROFILE / 'analysis/firmware/ppsspp-replacements/flash0/font')
    parser.add_argument('--font-manifest', type=Path, default=PROFILE / 'analysis/firmware/replacement-font-manifest.json')
    parser.add_argument('--webview2-sdk', type=Path, default=WEBVIEW2_SDK,
                        help='Extracted Microsoft.Web.WebView2 package used to build the launcher (licence source)')
    args = parser.parse_args()
    try:
        if args.output.exists() or linked(args.output):
            raise PackageError('Preserving existing package output; choose a new destination')
        entries = source_entries(args.build, args.font_directory, args.font_manifest,
            compiler=args.compiler, source_manifest=args.source_manifest, webview2_sdk=args.webview2_sdk)
        manifest = assemble(args.output, entries, runtime_archive(args.python_archive), ROOT / 'out')
        print(f'Source-only launcher/recompiler: {args.output}; {len(manifest["files"])} files. Game C++/EXE must be built locally from the player ISO.')
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, 'Player packaging failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
