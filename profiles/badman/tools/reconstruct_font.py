"""Reconstruct this hash-locked module at its verified fixed production base.

Consumes independently observed relocation field facts, not a general REL2
decoder. The recipe contains no executable words, destination words or assets.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

PROFILE = Path(__file__).resolve().parents[1]
RECIPE = PROFILE / 'config/font_relocations.json'
IDENTITY = PROFILE / 'config/font_module.json'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def integer(value, label, maximum=0xffffffff):
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError(f'{label} must be an integer field')
    try:
        result = int(value, 0) if isinstance(value, str) else value
    except ValueError as error:
        raise ValueError(f'Invalid {label}') from error
    if not 0 <= result <= maximum:
        raise ValueError(f'{label} is outside its bounded unsigned range')
    return result


def segments(source):
    if len(source) < 52:
        raise ValueError('Truncated PRX ELF header')
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', source)
    ident, kind, machine, version = header[:4]
    phoff, phsize, count = header[5], header[9], header[10]
    if ident[:7] != b'\x7fELF\x01\x01\x01' or kind != 0xffa0 or machine != 8 or version != 1:
        raise ValueError('Expected the externally decrypted/unpacked little-endian Allegrex PRX ELF')
    if phsize != 32 or not 0 < count <= 32 or phoff + count * phsize > len(source):
        raise ValueError('Invalid PRX program-header table')
    result = []
    for index in range(count):
        kind, offset, va, physical, file_size, memory_size, flags, align = struct.unpack_from('<8I', source, phoff + index * 32)
        if kind != 1:
            continue
        if (not 0 < memory_size <= 32 * 1024 * 1024 or file_size > memory_size or
                offset + file_size > len(source) or va + memory_size > 0x100000000 or
                align == 0 or align & (align - 1)):
            raise ValueError('Invalid/truncated load-segment extent')
        result.append({'file_offset': offset, 'vaddr': va, 'filesz': file_size,
                       'memsz': memory_size, 'flags': flags})
    if not result or sum(row['memsz'] for row in result) > 32 * 1024 * 1024:
        raise ValueError('No bounded load segments')
    for index, first in enumerate(result):
        for second in result[index + 1:]:
            if first['vaddr'] < second['vaddr'] + second['memsz'] and second['vaddr'] < first['vaddr'] + first['memsz']:
                raise ValueError('Overlapping PRX memory segments')
            if (first['filesz'] and second['filesz'] and first['file_offset'] < second['file_offset'] + second['filesz'] and
                    second['file_offset'] < first['file_offset'] + first['filesz']):
                raise ValueError('Overlapping PRX file segments')
    return result


def apply_operations(parts, layout, operations, base):
    """Apply only validated pointer/jump/immediate relocation additions."""
    base = integer(base, 'base')
    if len(parts) != len(layout) or not operations or len(operations) > 4096:
        raise ValueError('Invalid segment/relocation operation count')
    targets = []
    for row, part in zip(layout, parts):
        if len(part) != row['memsz'] or base + row['vaddr'] + row['memsz'] > 0x100000000:
            raise ValueError('Invalid relocation segment size/base')
        targets.append(base + row['vaddr'])
    seen, validated = set(), []
    for row in operations:
        if not isinstance(row, dict) or set(row) != {'segment', 'offset', 'mask', 'addend'}:
            raise ValueError('Relocation facts may contain only segment, offset, mask and addend')
        segment = integer(row['segment'], 'segment')
        offset, mask, addend = (integer(row[key], key) for key in ('offset', 'mask', 'addend'))
        if segment >= len(parts) or offset & 3 or offset + 4 > layout[segment]['filesz']:
            raise ValueError('Unaligned/out-of-file relocation field')
        if (segment, offset) in seen:
            raise ValueError('Overlapping/duplicate relocation fields')
        seen.add((segment, offset))
        if mask not in (0xffffffff, 0x03ffffff, 0x0000ffff) or addend > mask:
            raise ValueError('Unsupported relocation field mask/addend')
        original = struct.unpack_from('<I', parts[segment], offset)[0]
        opcode = original >> 26
        if mask == 0xffffffff:
            allowed = set(targets)  # pointer32: add a segment runtime base
        elif mask == 0x03ffffff:
            if opcode not in (2, 3):  # J/JAL's documented26-bit target field
                raise ValueError('Jump relocation field does not belong to J/JAL')
            allowed = {(target >> 2) & mask for target in targets}
        elif opcode == 15:  # LUI high16, with the observed address carry
            allowed = {((target >> 16) + carry) & mask for target in targets for carry in (0, 1)}
        else:
            # Only these public Allegrex opcode families occur in this module's
            # low16 facts: ADDIU, LW, LBU and LWC1. This is not a generic decoder.
            if opcode not in (9, 35, 36, 49):
                raise ValueError('Low16 relocation is outside the verified instruction families')
            allowed = {target & mask for target in targets}
        if addend not in allowed:
            raise ValueError('Recipe addend is not a permitted segment-base/carry relocation')
        validated.append((segment, offset, mask, addend, original))
    result = [bytearray(part) for part in parts]
    for segment, offset, mask, addend, original in validated:
        relocated = (original & ~mask) | (((original & mask) + addend) & mask)
        struct.pack_into('<I', result[segment], offset, relocated)
    return [bytes(part) for part in result]


def reconstruct(source, recipe, identity):
    required = {'schema', 'algorithm', 'source_sha256', 'load_base', 'relocation_count', 'provenance', 'operations'}
    if (not isinstance(recipe, dict) or set(recipe) != required or type(recipe['schema']) is not int or
            recipe['schema'] != 1 or recipe['algorithm'] != 'masked_modular_addition' or
            not isinstance(recipe['provenance'], str) or len(recipe['provenance']) > 1024):
        raise ValueError('Unsupported relocation-fact recipe schema')
    if recipe['source_sha256'] != identity['unpacked_sha256'] or sha(source) != identity['unpacked_sha256']:
        raise ValueError('Unpacked font source SHA256 does not match the supported module')
    base = integer(recipe['load_base'], 'load_base')
    if base != integer(identity['load_base'], 'profile load_base'):
        raise ValueError('Recipe base differs from the compiled font profile')
    if recipe['relocation_count'] != 902 or not isinstance(recipe['operations'], list) or len(recipe['operations']) != 902:
        raise ValueError('Expected exactly 902 verified relocation fields')
    layout = segments(source)
    if len(layout) != len(identity['snapshots']):
        raise ValueError('Source load-segment count differs from verified profile')
    parts = []
    for row, expected in zip(layout, identity['snapshots']):
        for key in ('vaddr', 'filesz', 'memsz', 'flags'):
            if row[key] != integer(expected[key], key):
                raise ValueError('Source segment layout differs from verified profile')
        parts.append(source[row['file_offset']:row['file_offset'] + row['filesz']] + bytes(row['memsz'] - row['filesz']))
    parts = apply_operations(parts, layout, recipe['operations'], base)
    for part, expected in zip(parts, identity['snapshots']):
        if sha(part) != expected['sha256']:
            raise ValueError('Reconstructed segment SHA256 differs from external verified relocation result')
    # Reuse our independent table validator and standard ELF wrapper, without
    # loading the user's ignored snapshots or invoking any analysis software.
    path = Path(__file__).with_name('prepare_font.py')
    spec = importlib.util.spec_from_file_location('badman_font_preparation', path)
    preparation = importlib.util.module_from_spec(spec)
    # Loading the normalizer must not add __pycache__ to the immutable bundle.
    # Compile trusted helper source in memory, as the setup/build loaders do.
    exec(compile(path.read_bytes(), str(path), 'exec'), preparation.__dict__)
    preparation.validate_tables(parts, identity)
    result = preparation.normalize(parts, identity)
    if sha(result) != identity['prepared_sha256']:
        raise ValueError('Final prepared font ELF SHA256 differs from verified translation input')
    return result


def reconstruct_file(unpacked, output):
    unpacked, output = Path(unpacked).resolve(), Path(output).absolute()
    if unpacked == output.resolve() or output.is_symlink():
        raise ValueError('Prepared output cannot overwrite/link to the unpacked source')
    if unpacked.stat().st_size > 1024 * 1024:
        raise ValueError('Unpacked input exceeds this bounded font-profile preparation size')
    source = unpacked.read_bytes()
    recipe, identity = json.loads(RECIPE.read_text()), json.loads(IDENTITY.read_text())
    result = reconstruct(source, recipe, identity)
    if output.exists():
        if not output.is_file() or output.stat().st_size != len(result) or output.read_bytes() != result:
            raise ValueError('Preserving an existing unrelated/different prepared output; choose a new file')
        return result
    output.parent.mkdir(parents=True, exist_ok=True)
    # Caller may use its own staging directory. Exclusive creation protects
    # against another process creating this path after the existence check.
    # A failed write can never become ready without the caller's hash check.
    with output.open('xb') as handle:
        handle.write(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unpacked', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = reconstruct_file(args.unpacked, args.output)
        print(f'Reconstructed 902 verified relocation fields: {args.output}; bytes={len(result)}, SHA256={sha(result)}', flush=True)
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f'Font reconstruction failed: {error}\n')


if __name__ == '__main__':
    main()
