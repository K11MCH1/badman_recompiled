"""Partition verified Ghidra instruction runs and invoke the generic generator.

Generated commercial-game translations stay in the ignored build directory.
No game-specific lowering is added to the framework.
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


def groups(path):
    result, group, size, end = [], [], 0, 0
    with path.open(encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            address, length = int(row["address"], 0), int(row["size"], 0)
            if address % 4 or length <= 0 or length > 0x4000 or length % 4 or address < end:
                raise ValueError(f"Invalid/overlapping code range: {row}")
            if group and size + length > 0x4000:
                result.append(group)
                group, size = [], 0
            group.append((f"badman_code_{address:08X}", address, length))
            size += length
            end = address + length
    if group:
        result.append(group)
    if not result:
        raise ValueError("Empty instruction map")
    return result


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def write_changed(path, text):
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--recompiler", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ranges", type=Path, default=PROFILE / "config/code_ranges.csv")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    partitions = groups(args.ranges)
    names = [f"badman_unit_{i:04}.cpp" for i in range(len(partitions))]
    names += ["badman_imports.cpp", "generated_registry.cpp"]
    if args.list:
        print(";".join(str(args.output / name).replace("\\", "/") for name in names))
        return
    if not args.elf or not args.recompiler:
        parser.error("--elf and --recompiler are required for generation")
    expected = json.loads((PROFILE / "config/executable.json").read_text())["elf_sha256"]
    if sha(args.elf) != expected:
        raise ValueError("ELF hash does not match the supported ULUS10456 executable")
    args.output.mkdir(parents=True, exist_ok=True)
    fingerprint = {"elf": expected, "ranges": sha(args.ranges),
                   "generator": sha(Path(__file__)), "recompiler": sha(args.recompiler)}
    manifest = args.output / "generation.json"
    if manifest.exists() and json.loads(manifest.read_text()).get("inputs") == fingerprint:
        if all((args.output / name).is_file() for name in names):
            print(f"Badman translations already current ({len(partitions)} units)")
            return
    with tempfile.TemporaryDirectory(prefix="badman-codegen-") as temporary:
        temp = Path(temporary)
        csv_path, cpp_path = temp / "functions.csv", temp / "generated.cpp"
        for i, partition in enumerate(partitions + [[]]):
            with csv_path.open("w", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(["name", "address", "size"])
                for name, address, length in partition:
                    writer.writerow([name, f"0x{address:08X}", f"0x{length:08X}"])
            subprocess.run([str(args.recompiler), str(args.elf), str(csv_path), str(cpp_path)],
                           check=True, capture_output=True, text=True)
            source = cpp_path.read_text()
            if partition:
                # Keep import wrappers in one separate unit. The root manual generator
                # intentionally emits them for every independent generated file.
                first = source.index("static void import_0(")
                registration = source.index("void register_generated_functions(")
                source = source[:first] + source[registration:]
                source = re.sub(r"^.*runtime\.register_function\([^\n]*&import_[^\n]*\n", "", source,
                                flags=re.MULTILINE)
                symbol = f"register_badman_unit_{i:04}"
                filename = names[i]
            else:
                symbol, filename = "register_badman_imports", "badman_imports.cpp"
            source = source.replace("void register_generated_functions(", f"void {symbol}(")
            write_changed(args.output / filename, source)
    symbols = [f"register_badman_unit_{i:04}" for i in range(len(partitions))]
    symbols += ["register_badman_imports"]
    registry = '#include "psprecomp/runtime.hpp"\nnamespace psprecomp {\n'
    registry += "".join(f"void {symbol}(Runtime &);\n" for symbol in symbols)
    registry += "void register_generated_functions(Runtime &rt) {\n"
    registry += "".join(f"    {symbol}(rt);\n" for symbol in symbols)
    registry += "}\n}\n"
    write_changed(args.output / "generated_registry.cpp", registry)
    manifest.write_text(json.dumps({"inputs": fingerprint, "translation_units": len(partitions),
                                   "instruction_runs": sum(map(len, partitions)),
                                   "instruction_count": sum(r[2] // 4 for g in partitions for r in g),
                                   "sources": names}, indent=2))
    print(f"Generated {len(partitions)} Badman units from verified instruction runs")


if __name__ == "__main__":
    main()
