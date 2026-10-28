"""Build the player's native executable locally from verified prepared game inputs.

The distributable supplies independent sources/tools, never a prebuilt game
translation. Commercial C++ and executables are confined to data_root/build.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import uuid

PRODUCER = 'PSPRecomp local native build'
SCHEMA = 1
OWNER = '.psprecomp-build-owner.json'
GENERATED_LIMIT = 70
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


class BuildError(Exception):
    def __init__(self, reason, message):
        self.reason = reason
        super().__init__(message)


def sha(path):
    with Path(path).open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def atomic_json(path, value):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        os.replace(temporary, path)
    finally:
        if temporary.is_file():
            temporary.unlink()


def read_json(path):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise BuildError('invalid_build_metadata', f'Cannot read build metadata {path}: {error}') from error


def source_root_default():
    script = Path(__file__).resolve()
    packaged = script.parent.parent / 'runtime-source'
    if packaged.is_dir():
        return packaged
    if script.parent.name == 'tools' and script.parent.parent.parent.name == 'profiles':
        return script.parents[3]
    raise BuildError('missing_runtime_source', 'Independent runtime-source folder is missing from the launcher installation')


def file_record(path):
    return {'path': str(path.resolve()), 'sha256': sha(path), 'size': path.stat().st_size}


def source_inventory(root):
    required = ['CMakeLists.txt', 'tools/codegen_main.cpp', 'tools/windows.manifest',
                'profiles/badman/CMakeLists.txt', 'profiles/badman/host/main.cpp',
                'profiles/badman/tools/generate.py', 'profiles/badman/tools/generate_font.py',
                'profiles/badman/config/executable.json', 'profiles/badman/config/font_module.json',
                'profiles/badman/config/code_ranges.csv', 'profiles/badman/config/font_code_ranges.csv']
    for relative in required:
        if not (root / relative).is_file():
            raise BuildError('missing_runtime_source', f'Required independent runtime source is missing: {relative}')
    paths = {root / 'CMakeLists.txt', root / 'profiles/badman/CMakeLists.txt'}
    for directory in ['include', 'src', 'tools', 'profiles/badman/host', 'profiles/badman/config']:
        for path in (root / directory).rglob('*'):
            if path == root / 'profiles/badman/config/source_bundle.json':
                continue  # Packaging inventory, not a compiler/profile input.
            if path.is_file() and (path.suffix.lower() in {'.cpp', '.c', '.h', '.hpp', '.cmake', '.json', '.csv', '.manifest', '.rc', '.py'} or
                    directory == 'profiles/badman/host' and path.suffix.lower() == '.hlsl'):
                paths.add(path)
    vendor = root / 'third_party/vulkan_headers'
    if vendor.exists() or (root / 'profiles/badman/host/badman_gpu_vulkan.cpp').is_file():
        if linked(vendor) or not vendor.is_dir() or any(linked(p) for p in vendor.rglob('*')):
            raise BuildError('unsafe_runtime_source', 'Missing or linked Vulkan-Headers source tree')
        actual = {p.relative_to(vendor).as_posix() for p in vendor.rglob('*') if p.is_file()}
        if actual != VULKAN_HEADERS_FILES:
            raise BuildError('missing_runtime_source', 'Vulkan-Headers must contain exactly the reviewed header and license inventory')
        paths.update(vendor / name for name in VULKAN_HEADERS_FILES)
    paths.update(root / relative for relative in required)
    result = {}
    for path in sorted(paths):
        if linked(path) or not path.resolve().is_relative_to(root):
            raise BuildError('unsafe_runtime_source', f'Linked or external runtime source: {path}')
        result[path.relative_to(root).as_posix()] = sha(path)
    package_manifest = root / 'source-manifest.json'
    if package_manifest.is_file():
        manifest = read_json(package_manifest)
        if manifest.get('schema') != 1 or manifest.get('game_independent_source_only') is not True:
            raise BuildError('invalid_runtime_source_manifest', 'Packaged runtime source manifest is invalid')
        recorded = {}
        for entry in manifest.get('files', []):
            relative = entry.get('path', '')
            path = root / relative
            if (not relative or Path(relative).is_absolute() or '..' in Path(relative).parts or relative in recorded or
                not path.is_file() or linked(path) or not path.resolve().is_relative_to(root) or
                sha(path) != entry.get('sha256') or path.stat().st_size != entry.get('bytes')):
                raise BuildError('runtime_source_changed', f'Packaged independent source failed manifest verification: {relative}')
            recorded[relative] = entry['sha256']
        if any(recorded.get(relative) != digest for relative, digest in result.items()):
            raise BuildError('runtime_source_changed', 'Packaged source manifest does not cover every build source')
        result['source-manifest.json'] = sha(package_manifest)
    return result


def ffmpeg_default(source):
    candidates = [source.parent / 'tools/ffmpeg']
    candidates += sorted((source / 'out/tools').glob('ffmpeg-*/*'))
    for candidate in candidates:
        if (candidate / 'include/libavcodec/avcodec.h').is_file():
            return candidate.resolve()
    raise BuildError('missing_ffmpeg_sdk', 'FFmpeg shared libraries and development headers are missing from the launcher tools')


def ffmpeg_inventory(prefix):
    required = ['include/libavcodec/avcodec.h', 'bin/avcodec-62.dll', 'bin/avutil-60.dll',
                'bin/swscale-9.dll', 'bin/swresample-6.dll']
    for relative in required:
        if not (prefix / relative).is_file():
            raise BuildError('missing_ffmpeg_sdk', f'FFmpeg dependency is missing: {prefix / relative}')
    paths = list((prefix / 'include').rglob('*.h')) + list((prefix / 'bin').glob('*.dll'))
    result = {}
    for path in sorted(paths):
        if linked(path) or not path.resolve().is_relative_to(prefix):
            raise BuildError('unsafe_ffmpeg_sdk', f'Linked FFmpeg dependency: {path}')
        result[path.relative_to(prefix).as_posix()] = sha(path)
    return result


def query(command):
    try:
        result = subprocess.run([str(item) for item in command], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding='utf-8', errors='replace',
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0, timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise BuildError('toolchain_query_failed', f'Cannot query build tool {command[0]}: {error}') from error
    if len(result.stdout) > 1024 * 1024:
        raise BuildError('toolchain_query_failed', 'Build tool returned oversized discovery output')
    return result.returncode, result.stdout


def discover_toolchain(cmake=None):
    if os.name != 'nt':
        raise BuildError('unsupported_build_host', 'This local game build currently requires Windows and Visual Studio 2022 C++ Build Tools')
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise BuildError('missing_msvc', 'Install Visual Studio 2022 Build Tools with Desktop development with C++, Windows SDK and CMake tools')
    code, text = query([vswhere, '-latest', '-products', '*', '-version', '[17.0,18.0)',
                        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json', '-utf8'])
    try:
        installations = json.loads(text) if code == 0 else []
        installation = installations[0]
        instance = Path(installation['installationPath']).resolve()
        compiler_dirs = sorted((instance / 'VC/Tools/MSVC').iterdir(), key=lambda path: tuple(int(x) for x in path.name.split('.')))
        vc = next(path for path in reversed(compiler_dirs) if (path / 'bin/Hostx64/x64/cl.exe').is_file())
    except (ValueError, IndexError, KeyError, OSError, StopIteration) as error:
        raise BuildError('missing_msvc', 'No complete Visual Studio 2022 x64 C++ compiler was found') from error
    bundled = instance / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    cmake_path = Path(cmake).expanduser().resolve() if cmake else Path(shutil.which('cmake') or bundled).resolve()
    if not cmake_path.is_file():
        raise BuildError('missing_cmake', 'CMake is required; install the Visual Studio C++ CMake tools component')
    code, version = query([cmake_path, '--version'])
    if code or not re.search(r'cmake version (\d+)\.(\d+)', version):
        raise BuildError('missing_cmake', 'Selected CMake executable did not report a valid version')
    match = re.search(r'cmake version (\d+)\.(\d+)', version)
    if tuple(map(int, match.groups())) < (3, 20):
        raise BuildError('old_cmake', 'CMake 3.20 or later is required')
    kits = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits/10'
    versions = sorted((path for path in (kits / 'Include').glob('*') if re.fullmatch(r'\d+(\.\d+)+', path.name)),
                      key=lambda path: tuple(int(x) for x in path.name.split('.')))
    sdk = next((path.name for path in reversed(versions) if (path / 'um/Windows.h').is_file() and
                (kits / 'Lib' / path.name / 'um/x64/kernel32.lib').is_file() and
                (kits / 'Lib' / path.name / 'ucrt/x64/ucrt.lib').is_file()), None)
    if not sdk:
        raise BuildError('missing_windows_sdk', 'A complete Windows 10/11 x64 SDK is required by the local compiler')
    tools = {'cmake': file_record(cmake_path), 'compiler': file_record(vc / 'bin/Hostx64/x64/cl.exe'),
             'linker': file_record(vc / 'bin/Hostx64/x64/link.exe'),
             'msbuild': file_record(instance / 'MSBuild/Current/Bin/MSBuild.exe'), 'python': file_record(Path(sys.executable))}
    return {'generator': 'Visual Studio 17 2022', 'architecture': 'x64', 'instance': str(instance),
            'vc_version': vc.name, 'sdk_version': sdk, 'cmake_version': version.splitlines()[0], 'tools': tools,
            'sdk_identity': {str(path.relative_to(kits)): sha(path) for path in [kits / 'Include' / sdk / 'um/Windows.h',
                kits / 'Include' / sdk / 'ucrt/stdio.h', kits / 'Lib' / sdk / 'um/x64/kernel32.lib', kits / 'Lib' / sdk / 'ucrt/x64/ucrt.lib']}}


def build_flags(elf, font_elf, prefix, jobs):
    return ['-DPSPRECOMP_PROFILE=badman', '-DPSPRECOMP_BUILD_TESTS=OFF', '-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF',
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
            '-DBADMAN_BUILD_NATIVE=ON', '-DBADMAN_BUILD_LAUNCHER=OFF', '-DPSPRECOMP_LTO=OFF', '-DPSPRECOMP_NATIVE_AVX2=OFF',
            '-DPSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH=OFF', '-DPSPRECOMP_AOT_PRODUCTION_FASTPATHS=OFF',
            f'-DPSPRECOMP_MSVC_MP_JOBS={jobs}', f'-DBADMAN_ELF={elf.as_posix()}',
            f'-DBADMAN_FONT_ELF={font_elf.as_posix()}', f'-DBADMAN_FFMPEG_PREFIX={prefix.as_posix()}',
            '-DBADMAN_FONT_DIRECTORY=', f'-DPython3_EXECUTABLE={Path(sys.executable).as_posix()}']


def pe_check(path):
    try:
        with path.open('rb') as handle:
            dos = handle.read(64)
            if len(dos) < 64 or dos[:2] != b'MZ':
                raise ValueError('missing DOS header')
            offset = struct.unpack_from('<I', dos, 60)[0]
            if offset > 1024 * 1024:
                raise ValueError('PE header offset exceeds bound')
            handle.seek(offset); header = handle.read(264)
            if len(header) < 264 or header[:4] != b'PE\0\0' or struct.unpack_from('<H', header, 4)[0] != 0x8664:
                raise ValueError('not an AMD64 PE executable')
            sections, optional_size, characteristics = struct.unpack_from('<H12xHH', header, 6)
            if not 1 <= sections <= 96 or optional_size < 112 or not characteristics & 2 or characteristics & 0x2000:
                raise ValueError('invalid executable header')
            if struct.unpack_from('<H', header, 24)[0] != 0x20B:
                raise ValueError('not a PE32+ executable')
            entry = struct.unpack_from('<I', header, 40)[0]
            size = struct.unpack_from('<I', header, 80)[0]
            if not 0 < entry < size:
                raise ValueError('invalid native entry point')
    except (OSError, ValueError, struct.error) as error:
        raise BuildError('invalid_native_artifact', f'Locally built executable is invalid: {path}: {error}') from error


def expected_layout(source):
    profile = source / 'profiles/badman'
    result = []
    for label, filename, ranges in [('main', 'generate.py', 'code_ranges.csv'), ('font', 'generate_font.py', 'font_code_ranges.csv')]:
        spec = importlib.util.spec_from_file_location('_badman_' + label + '_build_generator', profile / 'tools' / filename)
        module = importlib.util.module_from_spec(spec)
        # Query source-owned layout helpers without writing __pycache__ into
        # the immutable runtime-source distribution, including verify-only.
        exec(compile((profile / 'tools' / filename).read_text(encoding='utf-8'), str(profile / 'tools' / filename), 'exec'), module.__dict__)
        if label == 'font':
            identity = read_json(profile / 'config/font_module.json')
            groups = module.groups(profile / 'config' / ranges, base=int(identity['load_base'], 0), imports=identity['imports'])
        else:
            groups = module.groups(profile / 'config' / ranges)
        result.append(len(groups))
    if any(count <= 0 for count in result) or sum(result) > GENERATED_LIMIT:
        raise BuildError('invalid_translation_layout', 'Profile exceeds the bounded 70 translated-unit build')
    return result


def generated_receipt(build, source, elf_hash, font_hash, layout):
    recompiler = build / 'Release/psp_recomp.exe'
    if not recompiler.is_file():
        raise BuildError('missing_local_recompiler', 'CMake did not produce the local generic Allegrex recompiler')
    tool_hash = sha(recompiler)
    outputs = {}
    for label, folder, manifest_name, expected_hash, count, generator, ranges in [
        ('main', 'generated', 'generation.json', elf_hash, layout[0], 'generate.py', 'code_ranges.csv'),
        ('font', 'font_generated', 'font_generation.json', font_hash, layout[1], 'generate_font.py', 'font_code_ranges.csv')]:
        directory = build / 'profiles/badman' / folder
        manifest = read_json(directory / manifest_name)
        inputs = manifest.get('inputs', {})
        if (manifest.get('translation_units') != count or inputs.get('elf') != expected_hash or
            inputs.get('recompiler') != tool_hash or inputs.get('generator') != sha(source / 'profiles/badman/tools' / generator) or
            inputs.get('ranges') != sha(source / 'profiles/badman/config' / ranges)):
            raise BuildError('invalid_generation_receipt', f'{label} generated source identity/count does not match the local build')
        names = manifest.get('sources', [])
        if len(names) != count + 2 or len(set(names)) != len(names):
            raise BuildError('invalid_generation_receipt', f'{label} generated source list is incomplete')
        for name in names:
            path = directory / name
            if (Path(name).name != name or not name.endswith('.cpp') or not path.is_file() or linked(path) or
                not path.resolve().is_relative_to(build)):
                raise BuildError('invalid_generation_receipt', f'Unsafe/missing generated C++ source: {name}')
            outputs[path.relative_to(build).as_posix()] = sha(path)
        outputs[(directory / manifest_name).relative_to(build).as_posix()] = sha(directory / manifest_name)
    return {'recompiler_sha256': tool_hash, 'main_units': layout[0], 'font_units': layout[1], 'outputs': outputs}


def build_cache_manifest_path(data_root):
    return Path(data_root) / 'build/build-state.json'


def read_build_descriptor(data_root):
    return read_json(build_cache_manifest_path(data_root))


@contextmanager
def build_lock(root):
    if linked(root / '.build.lock'):
        raise BuildError('unsafe_build_directory', 'Build lock cannot be a symbolic link/junction')
    handle = (root / '.build.lock').open('a+b')
    try:
        handle.seek(0, 2)
        if not handle.tell():
            handle.write(b'\n'); handle.flush()
        handle.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise BuildError('build_busy', 'Another local game build is already using this data folder') from error
        yield
    finally:
        handle.close()


def own_directory(path, extra=None):
    if linked(path):
        raise BuildError('unsafe_build_directory', f'Build directory cannot be a link/junction: {path}')
    if path.exists():
        if not path.is_dir():
            raise BuildError('occupied_build_directory', f'Build path is occupied by a file: {path}')
        if any(path.iterdir()):
            owner = read_json(path / OWNER) if (path / OWNER).is_file() and not linked(path / OWNER) else None
            if owner != {'schema': SCHEMA, 'producer': PRODUCER, **(extra or {})}:
                raise BuildError('occupied_build_directory', f'Preserving unowned build directory: {path}')
            return
    path.mkdir(parents=True, exist_ok=True)
    atomic_json(path / OWNER, {'schema': SCHEMA, 'producer': PRODUCER, **(extra or {})})


def run_checked(command, log, emit, environment=None):
    log.write('\nCOMMAND ' + json.dumps([str(item) for item in command], ensure_ascii=False) + '\n'); log.flush()
    try:
        process = subprocess.Popen([str(item) for item in command], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', shell=False,
            env=environment,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    except OSError as error:
        raise BuildError('build_process_failed', f'Cannot start local compiler: {error}') from error
    try:
        while line := process.stdout.readline(65537):
            if len(line) > 65536:
                raise BuildError('build_output_invalid', 'Build process returned an oversized diagnostic line')
            log.write(line); log.flush(); emit(line.rstrip('\r\n'))
        if process.wait():
            raise BuildError('native_build_failed', f'Local compiler command failed with exit code {process.returncode}; see {log.name}')
    except BaseException:
        if process.poll() is None:
            process.kill(); process.wait()
        raise
    finally:
        process.stdout.close()


def ensure_build(data_root, elf, font_elf, source_root=None, ffmpeg_prefix=None, cmake=None, jobs=2, emit=print, verify_only=False):
    if not 1 <= jobs <= 4:
        raise BuildError('invalid_build_jobs', 'Local compiler parallelism must be between 1 and 4')
    source = (Path(source_root) if source_root else source_root_default()).expanduser().resolve()
    data = Path(data_root).expanduser().absolute()
    if linked(data):
        raise BuildError('unsafe_build_directory', 'Game data root cannot be a link/junction')
    data = data.resolve(); elf = Path(elf).expanduser().resolve(); font_elf = Path(font_elf).expanduser().resolve()
    profile = source / 'profiles/badman'
    inventory = source_inventory(source)
    identity = read_json(profile / 'config/executable.json'); font_identity = read_json(profile / 'config/font_module.json')
    for path, expected in [(elf, identity['elf_sha256']), (font_elf, font_identity['prepared_sha256'])]:
        if not path.is_file() or not path.is_relative_to(data) or sha(path) != expected:
            raise BuildError('unsupported_build_input', f'Missing/mismatched prepared ELF within this data folder: {path}')
    prefix = Path(ffmpeg_prefix).expanduser().resolve() if ffmpeg_prefix else ffmpeg_default(source)
    dependencies = ffmpeg_inventory(prefix); toolchain = discover_toolchain(cmake)
    layout = expected_layout(source); flags = build_flags(elf, font_elf, prefix, jobs)
    inputs = {'source_root': str(source), 'sources': inventory, 'backend_sha256': sha(Path(__file__)),
              'elf': file_record(elf), 'font_elf': file_record(font_elf), 'toolchain': toolchain,
              'python_version': sys.version, 'ffmpeg_prefix': str(prefix), 'ffmpeg': dependencies,
              'flags': flags, 'configuration': 'Release', 'layout': layout,
              'compiler_environment': {key: os.environ.get(key) for key in
                  ('CL', '_CL_', 'LINK', 'CXXFLAGS', 'CFLAGS', 'LDFLAGS', 'INCLUDE', 'LIB', 'LIBPATH', 'SOURCE_DATE_EPOCH')},
              'environment_policy': {'temporary': 'cmake/.tmp', 'python_utf8': True, 'python_bytecode': False}}
    fingerprint = hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    build_root = data / 'build'; entry = build_root / fingerprint; build = entry / 'cmake'
    executable = build / 'bin/Release/BadmanNative.exe'; manifest_path = entry / 'build.json'
    def verified_cache():
        try:
            if linked(build) or not executable.resolve().is_relative_to(build) or not manifest_path.is_file():
                return None
            manifest = read_json(manifest_path)
            if manifest.get('producer') != PRODUCER or manifest.get('fingerprint') != fingerprint or manifest.get('status') != 'ready':
                return None
            if (linked(executable) or not executable.is_file() or executable.stat().st_nlink != 1 or
                sha(executable) != manifest.get('executable_sha256')):
                return None
            pe_check(executable)
            receipt = generated_receipt(build, source, identity['elf_sha256'], font_identity['prepared_sha256'], layout)
            return manifest if receipt == manifest.get('generation') else None
        except BuildError:
            return None
    # Read-only verification must not claim directories, acquire locks or rewrite metadata.
    if verify_only:
        if linked(build_root) or linked(entry) or not verified_cache():
            raise BuildError('local_build_not_ready', 'No matching verified locally compiled game exists; run Prepare game to generate and compile it')
        emit(f'Verified locally compiled game: {executable}')
        return executable
    own_directory(build_root)
    with build_lock(build_root):
        own_directory(entry, {'fingerprint': fingerprint})
        if linked(build):
            raise BuildError('unsafe_build_directory', 'CMake build tree cannot be a link/junction')
        cached = verified_cache()
        if cached:
            emit('Reusing locally compiled game; source, toolchain, generated sources and executable hashes verified.')
        else:
            # An invalid cache must not be "rebuilt" by an up-to-date CMake
            # target accepting an altered old EXE. Preserve the entire old tree
            # and force genuinely fresh generation/compilation in a new one.
            if build.exists():
                if not build.resolve().is_relative_to(entry):
                    raise BuildError('unsafe_build_directory', 'CMake tree escapes its owned build entry')
                previous = entry / ('previous-' + uuid.uuid4().hex)
                try:
                    build.rename(previous)
                except OSError as error:
                    raise BuildError('build_cache_locked', 'Cannot preserve the old build tree; close the running game before rebuilding') from error
                emit(f'Preserved previous invalid/incomplete build: {previous}')
            logs = entry / 'logs'
            if linked(logs) or not logs.resolve().is_relative_to(entry):
                raise BuildError('unsafe_build_directory', 'Build diagnostics folder escapes its owned build entry')
            logs.mkdir(exist_ok=True)
            log_path = logs / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8] + '.log')
            manifest = {'schema': SCHEMA, 'producer': PRODUCER, 'fingerprint': fingerprint, 'inputs': inputs, 'status': 'building', 'log': str(log_path)}
            atomic_json(manifest_path, manifest)
            try:
                emit(f'Generating {layout[0]} main and {layout[1]} font Allegrex translation units, then compiling the local Windows game.')
                emit(f'Build diagnostics: {log_path}')
                temporary = build / '.tmp'; temporary.mkdir(parents=True, exist_ok=True)
                environment = os.environ.copy()
                environment.update(TEMP=str(temporary), TMP=str(temporary), PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')
                with log_path.open('w', encoding='utf-8', buffering=1) as log:
                    command = [toolchain['tools']['cmake']['path'], '-S', source, '-B', build, '-G', toolchain['generator'], '-A', 'x64',
                               '-T', 'host=x64,version=' + toolchain['vc_version'], '-DCMAKE_GENERATOR_INSTANCE=' + toolchain['instance'],
                               '-DCMAKE_SYSTEM_VERSION=' + toolchain['sdk_version'], *flags]
                    run_checked(command, log, emit, environment)
                    run_checked([toolchain['tools']['cmake']['path'], '--build', build, '--config', 'Release', '--target', 'BadmanNative', '--parallel', '1'], log, emit, environment)
                pe_check(executable)
                generation = generated_receipt(build, source, identity['elf_sha256'], font_identity['prepared_sha256'], layout)
                if (source_inventory(source) != inventory or sha(elf) != inputs['elf']['sha256'] or sha(font_elf) != inputs['font_elf']['sha256'] or
                    sha(Path(__file__)) != inputs['backend_sha256'] or ffmpeg_inventory(prefix) != dependencies or
                    any(sha(Path(tool['path'])) != tool['sha256'] for tool in toolchain['tools'].values())):
                    raise BuildError('build_inputs_changed', 'Source/input changed while compilation ran; the build is preserved but cannot become ready')
                manifest.update(status='ready', executable_sha256=sha(executable), generation=generation)
                atomic_json(manifest_path, manifest)
            except Exception as error:
                manifest.update(status='failed', reason=getattr(error, 'reason', 'build_failed'), error=str(error))
                atomic_json(manifest_path, manifest)
                emit(f'Local build failed. Preserved diagnostics: {log_path}')
                raise
        descriptor = {'schema': SCHEMA, 'producer': PRODUCER, 'fingerprint': fingerprint, 'executable': str(executable),
                      'sha256': sha(executable), 'entry_manifest': str(manifest_path)}
        pointer = build_cache_manifest_path(data)
        if not pointer.is_file() or read_json(pointer) != descriptor:
            atomic_json(pointer, descriptor)
        emit(f'Local native build verified: {executable}')
        return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-root', type=Path, required=True); parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--font-elf', type=Path, required=True); parser.add_argument('--source-root', type=Path)
    parser.add_argument('--ffmpeg-prefix', type=Path); parser.add_argument('--cmake', type=Path)
    parser.add_argument('--jobs', type=int, default=2); parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    try:
        ensure_build(args.data_root, args.elf, args.font_elf, args.source_root, args.ffmpeg_prefix, args.cmake,
                     args.jobs, lambda message: print(message, flush=True), args.verify_only)
    except (BuildError, OSError, ValueError) as error:
        parser.exit(1, f'Local build failed [{getattr(error, "reason", "build_failed")}]: {error}\n')


if __name__ == '__main__':
    main()
