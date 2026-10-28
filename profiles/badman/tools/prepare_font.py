"""Normalize verified external Allegrex-loader snapshots; no decryption/REL2 decoder.

The original sectionless PT_PSPREL2 PRX cannot be supplied directly to PSPRecomp.
Use the external preparation commands in progress/LIBFONT_RESEARCH.md first.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

PROFILE = Path(__file__).resolve().parents[1]
ROOT = PROFILE.parent.parent if PROFILE.parent.name.casefold() == 'profiles' else PROFILE
CONFIG = PROFILE / 'config/font_module.json'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def required(path, expected, label):
    if not path.is_file():
        raise ValueError(f'Missing {label}: {path}. External decryption/unpacking and isolated '
                         'Ghidra Allegrex relocation snapshots are prerequisites. See '
                         'profiles/badman/progress/LIBFONT_RESEARCH.md. The original ~SCE/REL2 '
                         'PRX is not a supported replacement for these prepared inputs.')
    data = path.read_bytes()
    if sha(data) != expected:
        raise ValueError(f'{label} SHA256 mismatch: {path}; expected {expected}, got {sha(data)}')
    return data


def validate_tables(parts, config):
    """Check relocated module pointers, exports/imports, and BSS independently."""
    base = int(config['load_base'], 0)

    def at(va, size):
        for spec, data in zip(config['snapshots'], parts):
            start = base + int(spec['vaddr'], 0)
            if start <= va and va + size <= start + len(data):
                return data[va-start:va-start+size]
        raise ValueError(f'Pointer outside module segments: 0x{va:08X}, size {size}')

    def word(va):
        return struct.unpack('<I', at(va, 4))[0]

    def string(va):
        result = bytearray()
        for offset in range(256):
            value = at(va+offset, 1)[0]
            if not value:
                return result.decode('ascii')
            result.append(value)
        raise ValueError('Unterminated module string')

    info = base + int(config['module_info_relative'], 0)
    attrs, version, name, gp, exp_begin, exp_end, imp_begin, imp_end = struct.unpack('<HH28sIIIII', at(info, 52))
    if name.split(b'\0', 1)[0].decode() != config['module'] or gp != int(config['gp'], 0):
        raise ValueError('Relocated module identity/GP mismatch')
    if (attrs, version, exp_begin, exp_end, imp_begin, imp_end) != (6, 0x101, base+0x7944, base+0x7968, base+0x7970, base+0x7998):
        raise ValueError('Unexpected module-info table bounds/attributes')
    actual_exports = []
    cursor = exp_begin
    while cursor < exp_end:
        lib, ver, flags, length, nvars, nfuncs, table = struct.unpack('<IHHBBHI', at(cursor,16))
        if length < 4 or cursor+length*4 > exp_end:
            raise ValueError('Invalid export record length')
        library = string(lib) if lib else 'syslib'
        total = nfuncs+nvars
        for index in range(total):
            actual_exports.append((library, word(table+index*4),
                                   word(table+(total+index)*4)-base,
                                   'function' if index < nfuncs else 'variable'))
        cursor += length*4
    expected_exports = [(r['library'], int(r['nid'],0), int(r['address_relative'],0), r['kind']) for r in config['exports']]
    if actual_exports != expected_exports:
        raise ValueError('Relocated exports do not match the verified NID/address map')
    actual_imports = []
    cursor = imp_begin
    while cursor < imp_end:
        lib, ver, flags, length, nvars, count, nids, stubs = struct.unpack('<IHHBBHII', at(cursor,20))
        if length < 5 or nvars or cursor+length*4 > imp_end:
            raise ValueError('Invalid import record length/variable imports')
        for index in range(count):
            at(stubs+index*8, 8)
            actual_imports.append((string(lib), word(nids+index*4), stubs+index*8-base))
        cursor += length*4
    expected_imports = [(r['library'], int(r['nid'],0), int(r['stub_relative'],0)) for r in config['imports']]
    if actual_imports != expected_imports:
        raise ValueError('Relocated imports do not match the verified NID/stub map')
    for spec, data in zip(config['snapshots'], parts):
        file_size, memory_size = int(spec['filesz'],0), int(spec['memsz'],0)
        if len(data) != memory_size or any(data[file_size:]):
            raise ValueError('Snapshot extent or zero-initialized BSS mismatch')


def normalize(parts, config):
    """Write standard fixed-address ELF, retaining absent-entry sentinel."""
    base = int(config['load_base'],0)
    names = b'\0.load0_head\0.sceModuleInfo\0.load0_tail\0.load1\0.shstrtab\0'
    offsets = {name:names.index(name.encode()+b'\0') for name in ('.load0_head','.sceModuleInfo','.load0_tail','.load1','.shstrtab')}
    off0 = 0x100
    off1 = (off0+len(parts[0])+15)&~15
    stroff = off1+int(config['snapshots'][1]['filesz'],0)
    shoff = (stroff+len(names)+3)&~3
    data = bytearray(shoff+6*40)
    ident = b'\x7fELF\x01\x01\x01'+bytes(9)
    struct.pack_into('<16sHHIIIIIHHHHHH',data,0,ident,2,8,1,int(config['entry'],0),52,shoff,0x10a23001,52,32,2,40,6,5)
    for index, (spec, segment, offset) in enumerate(zip(config['snapshots'],parts,(off0,off1))):
        va = base+int(spec['vaddr'],0)
        file_size = int(spec['filesz'],0)
        struct.pack_into('<8I',data,52+index*32,1,offset,va,va,file_size,int(spec['memsz'],0),spec['flags'],16)
        data[offset:offset+file_size] = segment[:file_size]
    data[stroff:stroff+len(names)] = names
    def section(index,name,kind,flags,va,offset,size,align):
        struct.pack_into('<10I',data,shoff+index*40,offsets[name],kind,flags,va,offset,size,0,0,align,0)
    section(1,'.load0_head',1,6,base,off0,0x799c,4)
    section(2,'.sceModuleInfo',1,2,base+0x799c,off0+0x799c,52,4)
    section(3,'.load0_tail',1,2,base+0x79d0,off0+0x79d0,len(parts[0])-0x79d0,4)
    section(4,'.load1',1,3,base+0x8020,off1,0x178,16)
    section(5,'.shstrtab',3,0,0,stroff,len(names),1)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module',type=Path,default=PROFILE/'game/PSP_GAME/USRDIR/data_usa/module/libfont.prx')
    parser.add_argument('--unpacked',type=Path,default=PROFILE/'analysis/libfont/libfont-unpacked.elf')
    parser.add_argument('--snapshots',type=Path,default=PROFILE/'analysis/libfont')
    parser.add_argument('--output',type=Path,default=ROOT/'out/badman/font/libfont-fixed.elf')
    args = parser.parse_args()
    try:
        config = json.loads(CONFIG.read_text())
        original = required(args.module,config['input_sha256'],'original libfont PRX')
        unpacked = required(args.unpacked,config['unpacked_sha256'],'externally unpacked ELF')
        if original[:4] != b'~SCE' or unpacked[:4] != b'\x7fELF':
            raise ValueError('Unexpected input containers')
        parts = [required(args.snapshots/r['file'],r['sha256'],'externally relocated snapshot') for r in config['snapshots']]
        validate_tables(parts,config)
        result = normalize(parts,config)
        if sha(result) != config['prepared_sha256']:
            raise ValueError('Normalizer output hash differs from verified fixed ELF')
        if not args.output.resolve().is_relative_to((ROOT/'out').resolve()):
            raise ValueError('Write prepared commercial input under the repository ignored out/ directory')
        args.output.parent.mkdir(parents=True,exist_ok=True)
        if not args.output.exists() or args.output.read_bytes() != result:
            temporary = args.output.with_suffix(args.output.suffix+'.tmp')
            temporary.write_bytes(result)
            temporary.replace(args.output)
        print(f'Prepared {args.output}: {len(result)} bytes, SHA256 {sha(result)}, GP {config["gp"]}')
    except (ValueError,OSError,struct.error) as error:
        parser.exit(1,f'Font preparation failed: {error}\n')


if __name__ == '__main__':
    main()
