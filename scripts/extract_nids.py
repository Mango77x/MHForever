#!/usr/bin/env python3
"""Builds platforms/psp/hle/nids.csv (library, NID, function name) from the PSPSDK stub libraries.

The PSPSDK (pspdev, BSD-licensed) ships one object per imported function: the stub lives in `.sceStub.text` under the
function's name and its NID sits at the matching index of `.rodata.sceNid`. Only user-mode libraries are kept.

usage: scripts/extract_nids.py [--pspdev /opt/pspdev] > platforms/psp/hle/nids.csv
"""
import argparse
import glob
import os
import re
import subprocess
import tempfile

EXCLUDED = re.compile(r"ForKernel|_driver|SystemCtrl|Vlf|Paf|Adrenaline|kubridge|Exploit|Ark|^lib|^VshCtrl$|^Lflash|"
                      r"^idsRegeneration$|^intrafont$|^pspUsbDevice$|^semaphore$|^chnnlsv$|^DcManager$", re.IGNORECASE)


def run(tool, *args, cwd=None):
    return subprocess.run([tool, *args], capture_output=True, text=True, cwd=cwd, check=False).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pspdev", default=os.environ.get("PSPDEV", "/opt/pspdev"))
    args = parser.parse_args()
    bindir = os.path.join(args.pspdev, "bin")
    ar, objdump = os.path.join(bindir, "psp-ar"), os.path.join(bindir, "psp-objdump")

    rows = set()
    with tempfile.TemporaryDirectory() as work:
        for archive in sorted(glob.glob(os.path.join(args.pspdev, "psp", "sdk", "lib", "*.a"))):
            target = os.path.join(work, os.path.basename(archive))
            os.makedirs(target)
            run(ar, "x", archive, cwd=target)
            # Archives whose objects are not named "<library>_NNNN.o" declare one library in their stub header.
            header_names = set()
            for obj in glob.glob(os.path.join(target, "*.o")):
                dump = run(objdump, "-s", "-j", ".rodata.sceResident", obj)
                text = b""
                for line in dump.splitlines():
                    m = re.match(r"^ [0-9a-f]{4} ((?:[0-9a-f]{2,8} ?)+)", line)
                    if m:
                        text += bytes.fromhex("".join(m.group(1).split()))
                if text.split(b"\0")[0]:
                    header_names.add(text.split(b"\0")[0].decode("latin1"))
            for obj in sorted(glob.glob(os.path.join(target, "*.o"))):
                stem = os.path.basename(obj)[:-2]
                m = re.match(r"^(.+)_\d{4}$", stem)
                if m and not m.group(1).startswith("__sceSas"):
                    library = m.group(1)
                elif stem.startswith("__sceSas"):
                    library = "sceSasCore"  # libpspsas names its objects after the functions
                elif len(header_names) == 1:
                    library = next(iter(header_names))
                else:
                    continue
                if EXCLUDED.search(library):
                    continue
                stubs = set()
                for line in run(objdump, "-t", obj).splitlines():
                    m = re.match(r"^([0-9a-f]{8}) .{7} (\S+)\s+[0-9a-f]{8} (\S+)$", line)
                    if m and m.group(2) == ".sceStub.text" and m.group(3) != ".sceStub.text":
                        stubs.add((int(m.group(1), 16), m.group(3)))
                if not stubs:
                    continue
                nids = []
                for line in run(objdump, "-s", "-j", ".rodata.sceNid", obj).splitlines():
                    m = re.match(r"^ [0-9a-f]{4} ((?:[0-9a-f]{8} ?){1,4})", line)
                    if m:
                        nids += [int.from_bytes(bytes.fromhex(w), "little") for w in m.group(1).split()]
                for offset, name in stubs:
                    if offset // 8 < len(nids):
                        rows.add((library, nids[offset // 8], name))

    print("# library,nid,name")
    print("# Generated from the PSPSDK stub libraries (pspdev, BSD-licensed) by scripts/extract_nids.py.")
    for library, nid, name in sorted(rows):
        print(f"{library},0x{nid:08X},{name}")


if __name__ == "__main__":
    main()
