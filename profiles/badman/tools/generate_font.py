"""Generate verified fixed-base libfont AOT code in an owned build directory.

Imports deliberately use Runtime::invoke_import (uncached): main-game cache slots
are a separate corpus. Public registration: psprecomp::register_badman_font_functions.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

PROFILE = Path(__file__).resolve().parents[1]
ROOT = PROFILE.parents[1]


def sha(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle,'sha256').hexdigest()


def linked(path):
    return path.is_symlink() or (hasattr(path,'is_junction') and path.is_junction())


def validate_output(output, allowed_root=None, names=()):
    """Default to ignored out/. External CMake builds require explicit ownership.

    This is an output confinement guard, not a permission to overwrite arbitrary
    files. The local-build orchestrator separately owns its dataRoot/build cache.
    """
    output=output.expanduser().absolute()
    allowed=(allowed_root if allowed_root is not None else ROOT/'out').expanduser().absolute()
    if linked(allowed):
        raise ValueError('Generated-code build root cannot be a link or junction')
    resolved_allowed=allowed.resolve()
    resolved_output=output.resolve()
    if resolved_output==resolved_allowed or not resolved_output.is_relative_to(resolved_allowed):
        raise ValueError('Generated code must stay inside its allowed build root')
    if allowed_root is not None:
        cache=allowed/'CMakeCache.txt'
        if linked(cache) or not cache.is_file():
            raise ValueError('Explicit generated-code root must be an existing configured CMake build directory')
        values={}
        for line in cache.read_text(encoding='utf-8',errors='strict').splitlines():
            if line and not line.startswith(('#','//')) and '=' in line:
                key,value=line.split('=',1);values[key.split(':',1)[0]]=value
        home=values.get('CMAKE_HOME_DIRECTORY')
        binary=values.get('CMAKE_CACHEFILE_DIR')
        if not home or not binary or Path(home).resolve()!=ROOT.resolve() or Path(binary).resolve()!=resolved_allowed:
            raise ValueError('Explicit build root CMake cache does not belong to this source tree and binary directory')
    cursor=output
    while cursor!=allowed:
        if linked(cursor):
            raise ValueError('Generated-code output contains a link or junction')
        if cursor.exists() and not cursor.is_dir():
            raise ValueError('Generated-code output component is not a directory')
        if cursor.parent==cursor:
            raise ValueError('Generated-code output does not descend from its allowed root')
        cursor=cursor.parent
    for name in names:
        target=output/name
        if target.name!=name or linked(target) or (target.exists() and (not target.is_file() or target.stat().st_nlink!=1)):
            raise ValueError('Generated-code target is linked or not an owned regular file: '+str(target))
    return resolved_output


def groups(path, maximum=0x2000, base=0x09fd0000, imports=()):
    result, group, used, end = [], [], 0, 0
    blocked = sorted((base+int(row['stub_relative'],0),base+int(row['stub_relative'],0)+8) for row in imports)
    with path.open(encoding='utf-8-sig',newline='') as handle:
        for row in csv.DictReader(handle):
            address,length = int(row['address'],0),int(row['size'],0)
            if address%4 or length<=0 or length%4 or address<end or address<base or address+length>base+0x7934:
                raise ValueError(f'Invalid/overlapping font instruction range: {row}')
            end = address+length
            # Raw JR RA/NOP import bodies must never become local labels: a
            # same-unit JAL would otherwise bypass the registered HLE wrapper.
            fragments = [(address,end)]
            for low,high in blocked:
                remaining = []
                for start,stop in fragments:
                    if high<=start or low>=stop:
                        remaining.append((start,stop))
                    else:
                        if start<low: remaining.append((start,low))
                        if high<stop: remaining.append((high,stop))
                fragments = remaining
            for address,stop in fragments:
                length = stop-address
                if any(address<high and low<stop for low,high in blocked):
                    raise ValueError('Import stub survived manual code-range exclusion')
                while length:
                    amount = min(length,maximum-used)
                    group.append((f'badman_font_code_{address:08X}',address,amount))
                    address += amount
                    length -= amount
                    used += amount
                    if used == maximum:
                        result.append(group)
                        group,used = [],0
    if group:
        result.append(group)
    if not result or sum(row[2]//4 for group in result for row in group) != 7727:
        raise ValueError('Font instruction map must contain 7727 translated instructions after excluding 22 import-stub instructions')
    return result


def write_changed(path,text):
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text,encoding='utf-8',newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf',type=Path)
    parser.add_argument('--recompiler',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--allowed-output-root',type=Path,
                        help='Explicit configured CMake binary root for a player-owned local build')
    parser.add_argument('--ranges',type=Path,default=PROFILE/'config/font_code_ranges.csv')
    parser.add_argument('--list',action='store_true')
    args = parser.parse_args()
    try:
        config = json.loads((PROFILE/'config/font_module.json').read_text())
        base = int(config['load_base'],0)
        partitions = groups(args.ranges,base=base,imports=config['imports'])
        import_pcs = {base+int(row['stub_relative'],0)+offset for row in config['imports'] for offset in (0,4)}
        names = [f'badman_font_unit_{i:04}.cpp' for i in range(len(partitions))]
        names += ['badman_font_imports.cpp','badman_font_registry.cpp']
        if args.list:
            print(';'.join(str(args.output/name).replace('\\','/') for name in names))
            return
        if not args.elf or not args.recompiler:
            parser.error('--elf and --recompiler are required for generation')
        args.output=validate_output(args.output,args.allowed_output_root,names+['font_generation.json'])
        if not args.elf.is_file():
            raise ValueError('Prepared font ELF is missing. Prepare the selected ISO with tools/setup_game.py first.')
        if sha(args.elf) != config['prepared_sha256']:
            raise ValueError('Expected the verified fixed-base font ELF (SHA256 '+config['prepared_sha256']+
                             '). The original ~SCE PRX and unpacked PT_PSPREL2 ELF cannot be used directly. '
                             'Run profiles/badman/tools/setup_game.py for the selected ISO.')
        args.output.mkdir(parents=True,exist_ok=True)
        fingerprint = {'elf':sha(args.elf),'ranges':sha(args.ranges),
                       'generator':sha(Path(__file__)),'recompiler':sha(args.recompiler),
                       'config':sha(PROFILE/'config/font_module.json')}
        manifest = args.output/'font_generation.json'
        if manifest.is_file():
            previous = json.loads(manifest.read_text())
            if previous.get('inputs') == fingerprint and all((args.output/name).is_file() and
                    previous.get('outputs',{}).get(name)==sha(args.output/name) for name in names):
                print(f'Font translations already current ({len(partitions)} units)')
                return
        with tempfile.TemporaryDirectory(prefix='badman-font-',dir=args.output) as directory:
            temporary = Path(directory)
            csv_path,cpp_path = temporary/'functions.csv',temporary/'generated.cpp'
            for index,partition in enumerate(partitions+[[]]):
                with csv_path.open('w',newline='') as handle:
                    writer = csv.writer(handle)
                    writer.writerow(['name','address','size'])
                    for name,address,length in partition:
                        writer.writerow([name,f'0x{address:08X}',f'0x{length:08X}'])
                subprocess.run([str(args.recompiler.resolve()),str(args.elf.resolve()),str(csv_path),str(cpp_path)],
                               check=True,capture_output=True,text=True)
                source = cpp_path.read_text()
                if partition:
                    first = source.index('static void import_0(')
                    registration = source.index('void register_generated_functions(')
                    source = source[:first]+source[registration:]
                    source,count = re.subn(r'^.*runtime\.register_function\([^\n]*&import_[^\n]*\n','',source,flags=re.MULTILINE)
                    if count != len(config['imports']):
                        raise ValueError('Generic generator import registration format changed')
                    symbol = f'register_badman_font_unit_{index:04}'
                    filename = names[index]
                else:
                    source,count = re.subn(r'\brt\.invoke_import_cached\(\d+u,\s*','rt.invoke_import(',source)
                    if count != len(config['imports']) or 'invoke_import_cached' in source:
                        raise ValueError('Generic generator import wrapper format changed; cannot ensure uncached imports')
                    symbol,filename = 'register_badman_font_imports','badman_font_imports.cpp'
                if source.count('void register_generated_functions(') != 1:
                    raise ValueError('Generic generator registration format changed')
                for pc in import_pcs:
                    if re.search(r'\bL_'+f'{pc:08X}'+r'\b',source,re.IGNORECASE):
                        raise ValueError(f'Raw import PC 0x{pc:08X} survived as a local label/jump')
                    if partition and re.search(r'runtime\.register_function\(0x'+f'{pc:08X}'+r'u\b',source,re.IGNORECASE):
                        raise ValueError(f'Raw import PC 0x{pc:08X} registered outside import wrappers')
                source = source.replace('void register_generated_functions(',f'void {symbol}(')
                write_changed(args.output/filename,source)
        symbols = [f'register_badman_font_unit_{i:04}' for i in range(len(partitions))]
        symbols += ['register_badman_font_imports']
        registry = '#include "psprecomp/runtime.hpp"\nnamespace psprecomp {\n'
        registry += ''.join(f'void {symbol}(Runtime &);\n' for symbol in symbols)
        registry += 'void register_badman_font_functions(Runtime &rt) {\n'
        registry += ''.join(f'    {symbol}(rt);\n' for symbol in symbols)
        registry += '}\n}\n'
        write_changed(args.output/'badman_font_registry.cpp',registry)
        unsupported = []
        for name in names:
            for address,word,reason in re.findall(r'rt\.unsupported\((0x[0-9A-F]+)u, (0x[0-9A-F]+)u, "([^"]+)"',
                                                 (args.output/name).read_text()):
                unsupported.append({'address':address,'word':word,'reason':reason})
        manifest.write_text(json.dumps({'inputs':fingerprint,'translation_units':len(partitions),
            'instruction_count':7727,'ghidra_instruction_count':7749,
            'excluded_import_stub_instructions':22,'excluded_import_stub_count':11,
            'import_stub_local_labels':0,'import_stub_manual_range_overlaps':0,
            'source_instruction_runs':6,'sources':names,
            'registration':'psprecomp::register_badman_font_functions','import_count':11,
            'import_dispatch':'uncached','instruction_unsupported_emissions':unsupported,
            'outputs':{name:sha(args.output/name) for name in names}},indent=2)+'\n')
        print(f'Generated {len(partitions)} font units, 11 uncached imports, 7727 translated instructions (22 raw import-stub instructions excluded)')
    except (ValueError,OSError,subprocess.CalledProcessError) as error:
        details = error.stderr if isinstance(error,subprocess.CalledProcessError) else str(error)
        parser.exit(1,f'Font generation failed: {details}\n')


if __name__ == '__main__':
    main()
