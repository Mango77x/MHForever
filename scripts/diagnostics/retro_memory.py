"""Local symbol, object and byte-diff tools. No game data is embedded.

Symbols are scoped by overlay filename: overlapping overlay addresses are
never silently merged. A nearest symbol is an annotation, not proof of ownership.
"""
from __future__ import annotations

import argparse
import bisect
from dataclasses import dataclass
import json
from pathlib import Path
import re
import struct
import tomllib

SYMBOL = re.compile(r"^\s*([^=]+?)\s*=\s*(0x[0-9a-fA-F]+)\s*;")


@dataclass(frozen=True)
class Symbol:
    address: int
    name: str
    scope: str


class Symbols:
    def __init__(self, records=()):
        self.records = sorted(set(records), key=lambda s: (s.scope, s.address, s.name))
        self.scopes = {}
        for s in self.records: self.scopes.setdefault(s.scope, []).append(s)
        self.addresses = {scope: [s.address for s in values] for scope, values in self.scopes.items()}

    @classmethod
    def load(cls, directory):
        directory = Path(directory)
        if directory.is_file() and directory.suffix == ".toml":
            with directory.open("rb") as stream:
                tables = tomllib.load(stream)
            records = []
            for scope in ("functions", "data"):
                for name, address in tables.get(scope, {}).items():
                    if type(address) is int:
                        records.append(Symbol(address, name, scope))
            if not records:
                raise ValueError("no integer function or data addresses found")
            return cls(records)
        files = [directory] if directory.is_file() else sorted(directory.rglob("*.symbol_addrs.txt"))
        if not files: raise ValueError("no symbol_addrs files found")
        scope_counts = {}
        for path in files:
            key=path.name.removesuffix(".symbol_addrs.txt")
            scope_counts[key]=scope_counts.get(key,0)+1
        records = []
        for path in files:
            scope = path.name.removesuffix(".symbol_addrs.txt")
            if scope_counts[scope]>1:
                scope=path.relative_to(directory).as_posix().removesuffix(".symbol_addrs.txt")
            for line in path.read_text(encoding="utf-8").splitlines():
                m = SYMBOL.match(line)
                if m: records.append(Symbol(int(m[2], 16), m[1], scope))
        return cls(records)

    def find(self, name, scope=None):
        return [s.__dict__ for s in self.records if name.casefold() in s.name.casefold() and (scope is None or s.scope == scope)]

    def nearest(self, address, scope):
        values = self.scopes.get(scope, [])
        i = bisect.bisect_right(self.addresses.get(scope, []), address) - 1
        if i < 0: return None
        symbol = values[i]
        return {**symbol.__dict__, "offset": address - symbol.address, "exact": address == symbol.address}


def objects(data, base, vtable, size=0x480, limit=128):
    if not 4 <= size <= 4096 or not 1 <= limit <= 512: raise ValueError("invalid object limits")
    needle = struct.pack("<I", vtable)
    found = []
    offset = 0
    while len(found) < limit:
        at = data.find(needle, offset)
        if at < 0: break
        offset = at + 1
        if (base + at) % 4 == 0 and at + size <= len(data): found.append(at)
    differences = []
    if len(found) > 1:
        for offset in range(0, size, 4):
            values = [data[at + offset:at + min(offset + 4, size)].hex() for at in found]
            if len(set(values)) > 1: differences.append({"offset": offset, "values": values})
    return {"vtable": vtable, "size": size, "addresses": [base + p for p in found],
            "limit_reached": len(found) == limit, "differing_fields": differences,
            "caution": "vtable matches are candidates; validate their lifetime and layout"}


def memory_diff(before, after, base=0x08800000, symbols=None, scope="eboot", regions=()):
    if len(before) != len(after): raise ValueError("memory dumps must have identical ranges")
    changes, i, total = [], 0, 0
    regions = sorted(regions, key=lambda r: r["size"])
    while i < len(before):
        if before[i] == after[i]: i += 1; continue
        start = i
        while i < len(before) and before[i] != after[i] and i - start < 32: i += 1
        address = base + start
        # Split at region boundaries so an annotation cannot span two structures.
        boundaries = [p for r in regions for p in (r["address"], r["address"] + r["size"]) if address < p < base + i]
        if symbols:
            addresses=symbols.addresses.get(scope,[])
            j=bisect.bisect_right(addresses,address)
            if j<len(addresses) and addresses[j]<base+i:boundaries.append(addresses[j])
        if boundaries: i = min(boundaries) - base
        owner = next((r["name"] for r in regions if r["address"] <= address < r["address"] + r["size"]), None)
        row = {"address": address, "length": i - start, "before": before[start:i].hex(), "after": after[start:i].hex(), "structure": owner}
        if symbols: row["nearest_symbol"] = symbols.nearest(address, scope)
        changes.append(row); total += i - start
    return {"base": base, "size": len(before), "changed_bytes": total, "changes": changes}


class ValueSearch:
    """Deterministic narrowed searches, usable on live dumps or saved trials."""
    def __init__(self, data, base, width, value):
        if width not in (1, 2, 4) or not 0 <= value < 1 << (8 * width): raise ValueError("invalid search value or width")
        self.base, self.width, self.size = base, width, len(data)
        self.values = {base + i: int.from_bytes(data[i:i + width], "little")
                       for i in range((-base) % width, len(data) - width + 1, width)
                       if int.from_bytes(data[i:i + width], "little") == value}

    def next(self, data, value=None, changed=None):
        if len(data) != self.size: raise ValueError("search range changed")
        kept = {}
        for address, old in self.values.items():
            i = address - self.base
            new = int.from_bytes(data[i:i + self.width], "little")
            if value is not None and new != value: continue
            if changed is not None and (new != old) != changed: continue
            kept[address] = new
        self.values = kept
        return dict(kept)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="operation", required=True)
    sym = sub.add_parser("sym"); sym.add_argument("directory", type=Path); sym.add_argument("name"); sym.add_argument("--scope")
    addr = sub.add_parser("addr"); addr.add_argument("directory", type=Path); addr.add_argument("address", type=lambda x:int(x,0)); addr.add_argument("--scope", required=True)
    obj = sub.add_parser("objects"); obj.add_argument("dump", type=Path); obj.add_argument("vtable", type=lambda x:int(x,0)); obj.add_argument("--base", type=lambda x:int(x,0), default=0x08800000); obj.add_argument("--size", type=lambda x:int(x,0), default=0x480)
    diff = sub.add_parser("diff"); diff.add_argument("before", type=Path); diff.add_argument("after", type=Path); diff.add_argument("--base", type=lambda x:int(x,0),default=0x08800000); diff.add_argument("--symbols", type=Path); diff.add_argument("--scope",default="eboot"); diff.add_argument("--regions",type=Path)
    p.add_argument("--out", type=Path)
    args = p.parse_args()
    if args.operation == "sym": result = Symbols.load(args.directory).find(args.name,args.scope)
    elif args.operation == "addr": result = Symbols.load(args.directory).nearest(args.address,args.scope)
    elif args.operation == "objects": result = objects(args.dump.read_bytes(),args.base,args.vtable,args.size)
    else: result = memory_diff(args.before.read_bytes(),args.after.read_bytes(),args.base,
            Symbols.load(args.symbols) if args.symbols else None,args.scope,
            json.loads(args.regions.read_text(encoding="utf-8")) if args.regions else ())
    text=json.dumps(result,indent=2,ensure_ascii=False)
    if args.out:
        with args.out.open("x",encoding="utf-8") as f:f.write(text+"\n")
    else: print(text)
    return 0


if __name__ == "__main__": raise SystemExit(main())
