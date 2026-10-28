"""Build the Badman Recompiled Android app (APK) from independent sources.

The APK holds the Java app with the Windows launcher's page and the app icon,
the shared runtime and JNI libraries built from
this repository, the NDK C++ runtime, a separately built LGPL FFmpeg, the
pinned open-licence replacement fonts, the phone-side preparation tools
(ISO extractor, recompiler, preparation tool, external decryptor) and
Termux's Clang/LLD with the NDK headers they compile against. It never
contains a game library, generated translation or game data: the phone
produces those from the player's own ISO.

build_apk.py --ndk-build <NDK build dir> --ffmpeg <FFmpeg Android lib dir> --ffmpeg-license <COPYING.LGPLv2.1>
             --pspdecrypt <Android pspdecrypt> --pspdecrypt-license <LICENSE.TXT>
             --compiler-packages <Termux package dir> --compiler-manifest <COMPILER_PACKAGES.json>
             --output app.apk [--debuggable] [--abi arm64-v8a] [--sdk ...] [--ndk ...] [--jdk ...]
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

PROFILE = Path(__file__).resolve().parents[2]
ROOT = PROFILE.parents[1]
APP = PROFILE / 'android'
TRIPLES = {'arm64-v8a': 'aarch64-linux-android', 'armeabi-v7a': 'arm-linux-androideabi'}
CLANG_TARGETS = {'arm64-v8a': 'aarch64-linux-android', 'armeabi-v7a': 'armv7a-linux-androideabi'}
TERMUX_ARCH = {'arm64-v8a': 'aarch64', 'armeabi-v7a': 'arm'}
BUILTINS = {'arm64-v8a': 'aarch64', 'armeabi-v7a': 'arm'}
NATIVE = ('libbadman_runtime.so', 'libbadman_android.so')
# Phone-side preparation tools from the NDK build, run from the read-only library folder.
TOOLS = {'psp_iso_extract': 'libpsp_iso_extract.so', 'badman_prepare': 'libbadman_prepare.so',
         'psp_recomp': 'libpsp_recomp.so'}
COMPILER_SKIP = ('libclang_rt.', 'libarcher', 'libLTO', 'libRemarks', 'libclang.so', 'libomp')
FFMPEG = ('libavcodec.so', 'libavutil.so', 'libswresample.so', 'libswscale.so')
FORBIDDEN = ('libbadman_game.so',)
MIN_SDK, TARGET_SDK = 29, 34


class BuildError(Exception):
    pass


def digest(path):
    with Path(path).open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def run(arguments, log):
    result = subprocess.run([str(a) for a in arguments], capture_output=True, text=True, encoding='utf-8', errors='replace')
    log.write('$ ' + ' '.join(str(a) for a in arguments) + '\n' + result.stdout + result.stderr + '\n')
    if result.returncode:
        raise BuildError(f'{Path(arguments[0]).name} failed ({result.returncode}): {(result.stdout + result.stderr).strip()[-800:]}')
    return result.stdout


def default_path(environment, fallback):
    value = os.environ.get(environment)
    return Path(value) if value else Path(fallback)


def font_rows(font_directory, font_manifest):
    rows = json.loads(Path(font_manifest).read_text(encoding='utf-8')).get('files', [])
    expected = {'jpn0.pgf', *(f'ltn{i}.pgf' for i in range(8))}
    if len(rows) != 9 or {row.get('name') for row in rows} != expected:
        raise BuildError('Expected exactly the nine pinned open-licence replacement fonts')
    for row in rows:
        source = Path(font_directory) / row['name']
        if not source.is_file() or digest(source) != row.get('sha256') or source.stat().st_size != row.get('size'):
            raise BuildError('Replacement font identity mismatch: ' + row['name'])
    return [(Path(font_directory) / row['name'], 'assets/fonts/' + row['name']) for row in rows]


def find_tool(build_directory, name):
    matches = [p for p in (build_directory / name, build_directory / 'bin/Release' / name, build_directory / 'bin' / name)
               if p.is_file()]
    if not matches:
        raise BuildError(f'{name} was not found in the NDK build {build_directory}')
    return matches[0]


def compiler_payload(args, llvm, exe, log):
    """Termux's Clang 21 and LLD for the phone, each file checked against the signed package index."""
    arch = TERMUX_ARCH[args.abi]
    manifest = json.loads(args.compiler_manifest.read_text(encoding='utf-8'))
    if not str(manifest.get('status', '')).startswith('PASS_SIGNED_INDEX'):
        raise BuildError('The compiler package manifest is not a verified signed-index extraction')
    packages = manifest['packages']
    base = args.compiler_packages / arch

    def verified(path):
        relative = path.relative_to(base)
        key = '/'.join(relative.parts[1:])
        row = packages.get(f"{arch}/{relative.parts[0].removesuffix('-files')}", {}).get('files', {}).get(key)
        if not row or digest(path) != row['sha256']:
            raise BuildError(f'Compiler package file does not match the signed index: {relative}')
        return path

    libraries, aliases = [], {}
    for path in sorted(base.glob('*-files/lib/*')):
        if not path.is_file() or not (path.name.endswith('.so') or '.so.' in path.name) or path.name.startswith(COMPILER_SKIP):
            continue
        verified(path)
        dynamic = run([llvm / f'bin/llvm-readelf{exe}', '--dynamic', path], log)
        match = re.search(r'\(SONAME\).*?\[(.*?)\]', dynamic)
        soname = match.group(1) if match else path.name
        # Versioned file names cannot be APK libraries, and Termux's libc++ must
        # not replace the NDK one the game uses: those get a private file name,
        # and the phone links the soname to it in the compiler's alias folder.
        if path.name.endswith('.so') and path.name != 'libc++_shared.so':
            name = path.name
        else:
            name = 'libpsp_dep_' + re.sub(r'[^A-Za-z0-9]+', '_', soname.removeprefix('lib').split('.so')[0]) + '.so'
        if name != soname:
            aliases[soname] = name
        libraries.append((path, name))
    for package, binary, name in (('clang', 'clang-21', 'libpsp_clang_tool.so'), ('lld', 'lld', 'libpsp_lld_tool.so')):
        libraries.append((verified(base / f'{package}-files/bin/{binary}'), name))
    notices = [(verified(p), 'assets/licenses/compiler/' + p.relative_to(base).as_posix())
               for p in sorted(base.glob('*-files/share/doc/**/*'))
               if p.is_file() and any(k in p.name.lower() for k in ('license', 'copying', 'copyright', 'notice'))]
    return libraries, aliases, notices


def toolchain_archive(args, llvm, aliases, work):
    """Headers, link stubs and sources the phone compiles the game against."""
    triple = TRIPLES[args.abi]
    sysroot = llvm / 'sysroot'
    entries = []

    def tree(directory, prefix):
        for path in sorted(directory.rglob('*')):
            if path.is_file():
                entries.append((path, prefix + path.relative_to(directory).as_posix()))
    tree(sysroot / 'usr/include', 'sysroot/usr/include/')
    tree(sysroot / 'usr/lib' / triple / str(MIN_SDK), f'sysroot/usr/lib/{triple}/{MIN_SDK}/')
    builtins = sorted(llvm.glob(f'lib/clang/*/lib/linux/libclang_rt.builtins-{BUILTINS[args.abi]}-android.a'))
    if not builtins:
        raise BuildError('NDK compiler-rt builtins were not found')
    entries.append((builtins[-1], 'builtins/' + builtins[-1].name))
    # The unwinder NDK Clang links into every shared library (`-l:libunwind.a`).
    unwind = builtins[-1].parent / BUILTINS[args.abi] / 'libunwind.a'
    if not unwind.is_file():
        raise BuildError(f'NDK libunwind.a was not found at {unwind}')
    entries.append((unwind, 'builtins/libunwind.a'))
    resource = sorted((args.compiler_packages / TERMUX_ARCH[args.abi] / 'clang-files/lib/clang').glob('*/include'))
    if not resource:
        raise BuildError('Clang resource headers were not found in the compiler packages')
    tree(resource[-1], 'resource/include/')
    tree(ROOT / 'include', 'include/')
    entries.append((PROFILE / 'host/game_entry.cpp', 'source/game_entry.cpp'))
    description = {'target': f'{CLANG_TARGETS[args.abi]}{MIN_SDK}', 'triple': triple, 'api': MIN_SDK,
                   'builtins': 'builtins/' + builtins[-1].name, 'unwind': 'builtins/libunwind.a', 'aliases': aliases}
    archive = work / 'toolchain.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for source, name in entries:
            bundle.write(source, name)
        bundle.writestr('toolchain.json', json.dumps(description, indent=1))
    return archive


# Bump when the phone's compile or link commands change in a way that needs a rebuild.
GAME_BUILD_RECIPE = 1


def game_interface():
    """What a phone-built game library depends on: the framework headers it is compiled
    against, its entry source and the build recipe. Runtime changes behind that
    interface do not need the game compiled again."""
    hasher = hashlib.sha256(f'recipe={GAME_BUILD_RECIPE}\n'.encode())
    for path in sorted([*(p for p in (ROOT / 'include').rglob('*') if p.is_file()), PROFILE / 'host/game_entry.cpp']):
        # Line endings do not change what the compiler sees.
        hasher.update(path.relative_to(ROOT).as_posix().encode() + b'\0' + path.read_bytes().replace(b'\r\n', b'\n') + b'\0')
    return hasher.hexdigest()


def profile_facts(work):
    """The identities the phone checks while preparing: ISO, decrypted executable, font
    module, and the game library interface."""
    executable = json.loads((PROFILE / 'config/executable.json').read_text(encoding='utf-8'))
    font = json.loads((PROFILE / 'config/font_module.json').read_text(encoding='utf-8'))
    facts = {'iso_sha256': executable['iso_sha256'], 'iso_size': executable['iso_size'],
             'elf_sha256': executable['elf_sha256'], 'font_sha256': font['prepared_sha256'],
             'game_interface': game_interface()}
    path = work / 'profile.json'
    path.write_text(json.dumps(facts, indent=1) + '\n', encoding='utf-8')
    return path


def build(args):
    sdk, ndk, jdk = args.sdk, args.ndk, args.jdk
    platform = sdk / f'platforms/android-{TARGET_SDK}/android.jar'
    tools = sorted((sdk / 'build-tools').glob('*'))
    if not platform.is_file() or not tools:
        raise BuildError(f'Android SDK platform {TARGET_SDK} and build-tools are required under {sdk}')
    build_tools = tools[-1]
    exe = '.exe' if os.name == 'nt' else ''
    llvm = next((ndk / 'toolchains/llvm/prebuilt').glob('*'), None)
    if llvm is None:
        raise BuildError(f'Android NDK toolchain not found under {ndk}')
    work = args.output.parent / (args.output.stem + '-work')
    if work.exists():
        shutil.rmtree(work)
    (work / 'classes').mkdir(parents=True)
    (work / 'dex').mkdir()
    log = (work / 'build.log').open('w', encoding='utf-8')

    libraries = []
    for name in NATIVE:
        libraries.append((find_tool(args.ndk_build, name), name))
    for tool, name in TOOLS.items():
        libraries.append((find_tool(args.ndk_build, tool), name))
    libraries.append((args.pspdecrypt, 'libpspdecrypt.so'))
    libraries.append((llvm / 'sysroot/usr/lib' / TRIPLES[args.abi] / 'libc++_shared.so', 'libc++_shared.so'))
    for name in FFMPEG:
        libraries.append((args.ffmpeg / name, name))
    for source, name in libraries:
        if not source.is_file():
            raise BuildError(f'Missing native library {source}')
        if name in FORBIDDEN:
            raise BuildError(f'{name} is built on the phone and must never be packaged')
    # Our own libraries ship without their static symbol tables; the dynamic
    # symbols the phone-built game library links against stay.
    (work / 'lib').mkdir()
    for index, (source, name) in enumerate(libraries):
        if name in NATIVE or name in TOOLS.values():
            stripped = work / 'lib' / name
            run([llvm / f'bin/llvm-strip{exe}', '--strip-unneeded', '-o', stripped, source], log)
            libraries[index] = (stripped, name)
    compiler, aliases, notices = compiler_payload(args, llvm, exe, log)
    libraries.extend(compiler)
    if len({name for _, name in libraries}) != len(libraries):
        raise BuildError('Two native libraries would share one APK file name')

    sources = sorted((APP / 'java').rglob('*.java'))
    run([jdk / f'bin/javac{exe}', '-source', '8', '-target', '8', '-encoding', 'UTF-8', '-Xlint:-options',
         '-bootclasspath', platform, '-classpath', build_tools / 'core-lambda-stubs.jar',  # lambdas, desugared by d8
         '-d', work / 'classes', *sources], log)
    run([jdk / f'bin/java{exe}', '-cp', build_tools / 'lib/d8.jar', 'com.android.tools.r8.D8', '--release',
         '--min-api', MIN_SDK, '--lib', platform, '--output', work / 'dex', *sorted((work / 'classes').rglob('*.class'))], log)
    base = work / 'base.apk'
    manifest = APP / 'AndroidManifest.xml'
    if args.package_id:
        # A development install beside the player's app: its own application
        # id and label, so it never replaces that app or its prepared game.
        manifest = work / 'AndroidManifest.xml'
        label = 'Badman Recompiled (' + args.package_id.rsplit('.', 1)[-1] + ')'
        manifest.write_text((APP / 'AndroidManifest.xml').read_text(encoding='utf-8')
                            .replace('android:label="Badman Recompiled"', f'android:label="{label}"'), encoding='utf-8')
    # The launcher icon (and its background colour) are the only resources.
    resources = work / 'resources.zip'
    run([build_tools / f'aapt2{exe}', 'compile', '--dir', APP / 'res', '-o', resources], log)
    link = [build_tools / f'aapt2{exe}', 'link', '--manifest', manifest, '-I', platform,
            '--min-sdk-version', MIN_SDK, '--target-sdk-version', TARGET_SDK, '-o', base, resources]
    if args.package_id:
        link += ['--rename-manifest-package', args.package_id]
    if args.debuggable:
        link.append('--debug-mode')
    run(link, log)

    extra = font_rows(args.font_directory, args.font_manifest)
    # The launcher page is the Windows launcher's own HTML/CSS/JavaScript.
    for name in ('index.html', 'launcher.css', 'launcher.js', 'hand.svg', 'fonts/PixelifySans.ttf', 'fonts/OFL.txt'):
        extra.append((PROFILE / 'launcher-ui' / name, 'assets/launcher/' + name))
    extra.append((args.font_directory.parents[1] / 'UME-FONT-LICENSE.txt', 'assets/licenses/UME-FONT-LICENSE.txt'))
    extra.append((ROOT / 'LICENSE', 'assets/licenses/PSPRecomp-MIT.txt'))
    extra.append((args.ffmpeg_license, 'assets/licenses/FFmpeg-COPYING.LGPLv2.1.txt'))
    extra.append((ndk / 'NOTICE', 'assets/licenses/Android-NDK-NOTICE.txt'))
    extra.append((ndk / 'NOTICE.toolchain', 'assets/licenses/Android-NDK-NOTICE.toolchain.txt'))
    extra.append((args.pspdecrypt_license, 'assets/licenses/pspdecrypt-LICENSE.txt'))
    extra.extend(notices)
    extra.append((profile_facts(work), 'assets/profile.json'))
    toolchain = toolchain_archive(args, llvm, aliases, work)
    unsigned = work / 'unsigned.apk'
    shutil.copy2(base, unsigned)
    contents = []
    with zipfile.ZipFile(unsigned, 'a') as apk:
        apk.write(work / 'dex/classes.dex', 'classes.dex', compress_type=zipfile.ZIP_DEFLATED)
        for source, name in libraries:
            apk.write(source, f'lib/{args.abi}/{name}', compress_type=zipfile.ZIP_DEFLATED)
            contents.append({'path': f'lib/{args.abi}/{name}', 'sha256': digest(source)})
        for source, name in extra:
            if not Path(source).is_file():
                raise BuildError(f'Missing package file {source}')
            apk.write(source, name, compress_type=zipfile.ZIP_DEFLATED)
            contents.append({'path': name, 'sha256': digest(source)})
        # Already compressed; stored so the app can stream it out.
        apk.write(toolchain, 'assets/toolchain.zip', compress_type=zipfile.ZIP_STORED)
        contents.append({'path': 'assets/toolchain.zip', 'sha256': digest(toolchain)})
    aligned = work / 'aligned.apk'
    run([build_tools / f'zipalign{exe}', '-P', '16', '-f', '4', unsigned, aligned], log)
    # One local key for every build, so updates install over each other.
    keystore = args.keystore or ROOT / 'out/android/badman-debug.keystore'
    keystore.parent.mkdir(parents=True, exist_ok=True)
    if not keystore.exists():
        if args.keystore:
            raise BuildError(f'Keystore not found: {keystore}')
        run([jdk / f'bin/keytool{exe}', '-genkeypair', '-keystore', keystore, '-storepass', 'android', '-keypass', 'android',
             '-alias', 'androiddebugkey', '-dname', 'CN=Badman Recomp local build', '-validity', '10000',
             '-keyalg', 'RSA', '-keysize', '2048'], log)
    run([jdk / f'bin/java{exe}', '-jar', build_tools / 'lib/apksigner.jar', 'sign', '--ks', keystore,
         '--ks-pass', 'pass:' + args.keystore_password, '--key-pass', 'pass:' + args.keystore_password,
         '--out', args.output, aligned], log)
    run([jdk / f'bin/java{exe}', '-jar', build_tools / 'lib/apksigner.jar', 'verify', args.output], log)
    log.close()
    report = {'apk': str(args.output), 'sha256': digest(args.output), 'bytes': args.output.stat().st_size,
              'abi': args.abi, 'debuggable': args.debuggable, 'min_sdk': MIN_SDK, 'target_sdk': TARGET_SDK,
              'contents': contents, 'game_content': False}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    print(json.dumps({'apk': report['apk'], 'sha256': report['sha256'], 'bytes': report['bytes']}))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ndk-build', type=Path, required=True, help='NDK build with the runtime, JNI library and tools')
    parser.add_argument('--pspdecrypt', type=Path, required=True, help='Android pspdecrypt (tools/android/build_pspdecrypt_android.sh)')
    parser.add_argument('--pspdecrypt-license', type=Path, required=True)
    parser.add_argument('--compiler-packages', type=Path, required=True, help='Extracted Termux clang/lld packages by architecture')
    parser.add_argument('--compiler-manifest', type=Path, required=True, help='Signed-index verification of those packages')
    parser.add_argument('--ffmpeg', type=Path, required=True, help='Android FFmpeg lib directory (tools/build_ffmpeg_android.sh)')
    parser.add_argument('--ffmpeg-license', type=Path, required=True, help="FFmpeg's COPYING.LGPLv2.1")
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--abi', choices=sorted(TRIPLES), default='arm64-v8a')
    parser.add_argument('--debuggable', action='store_true', help='Local engineering build (adb run-as)')
    parser.add_argument('--package-id', help='Development only: install beside the player app under this application id')
    parser.add_argument('--keystore', type=Path)
    parser.add_argument('--keystore-password', default='android')
    parser.add_argument('--sdk', type=Path, default=default_path('ANDROID_SDK_ROOT', Path.home() / 'AppData/Local/Android/Sdk'))
    parser.add_argument('--ndk', type=Path, default=default_path('ANDROID_NDK_ROOT', Path.home() / 'AppData/Local/Android/Sdk/ndk/27.3.13750724'))
    parser.add_argument('--jdk', type=Path, default=default_path('JAVA_HOME', Path.home() / 'AppData/Local/Programs/DroidDeckJDK17/jdk-17.0.20.1+1'))
    parser.add_argument('--font-directory', type=Path, default=PROFILE / 'analysis/firmware/ppsspp-replacements/flash0/font')
    parser.add_argument('--font-manifest', type=Path, default=PROFILE / 'analysis/firmware/replacement-font-manifest.json')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        build(args)
    except BuildError as error:
        print('APK build failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
