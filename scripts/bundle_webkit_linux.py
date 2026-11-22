#!/usr/bin/env python3
"""Bundle WebKitGTK 4.1 with the Linux release, for systems that lack it (the Steam Deck's SteamOS).

  bundle_webkit_linux.py --out <dir>

Writes <dir>/lib/webkit (WebKitGTK, its helper programs, the libraries they need that GTK itself does not bring, and
GIO's TLS module) and <dir>/licenses/webkitgtk (the copyright file of every package they come from, with its version).
package_release.py --webkit-bundle <dir> puts both in the archive.

Run it on the system that builds the release (Ubuntu 24.04 with libwebkit2gtk-4.1-0 and glib-networking): it copies
that system's files. The launcher's first program (launcher/native/starter_linux.cpp) uses them only where the system
has no WebKitGTK of its own, and of the libraries only those the system lacks. The graphics stack, the C and C++
runtimes and GTK's own libraries are never bundled: they must be the system's.

WebKit starts its helper programs from a folder written into it when it was built (here
/usr/lib/x86_64-linux-gnu/webkit2gtk-4.1). The bundled copy gets the starter's kRuntimeDir there instead, a path of the
same length that the starter links to the bundled helpers.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
STARTER = ROOT / "launcher/native/starter_linux.cpp"
WEBKIT = "libwebkit2gtk-4.1.so.0"
JAVASCRIPTCORE = "libjavascriptcoregtk-4.1.so.0"
HELPERS = ("WebKitWebProcess", "WebKitNetworkProcess", "WebKitGPUProcess")
INJECTED_BUNDLE = "injected-bundle/libwebkit2gtkinjectedbundle.so"

# Always the system's: the C and C++ runtimes, the graphics stack (bundling any of it breaks the GPU drivers), and
# what talks to the system's services.
SYSTEM = re.compile(r"^(ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libanl|libstdc\+\+|libgcc_s|"
                    r"libatomic|libGL|libEGL|libGLX|libGLdispatch|libOpenGL|libGLES.*|libgbm|libdrm.*|libwayland-.*|"
                    r"libX.*|libxcb.*|libxkbcommon.*|libepoxy|libudev|libsystemd|libdbus-1)\.so")
# GTK 3 and what every GTK 3 needs: the launcher's window links GTK, so a system that starts it has them.
GTK = re.compile(r"^(libgtk-3|libgdk-3|libglib-2\.0|libgio-2\.0|libgobject-2\.0|libgmodule-2\.0|libgthread-2\.0|"
                 r"libpango-1\.0|libpangocairo-1\.0|libpangoft2-1\.0|libcairo|libcairo-gobject|libharfbuzz|"
                 r"libfontconfig|libfreetype|libgdk_pixbuf-2\.0|libatk-1\.0|libatk-bridge-2\.0|libatspi|libpixman-1|"
                 r"libfribidi|libpng16|libexpat|libffi|libpcre2-8|libz)\.so")


def runtime_dir() -> str:
    match = re.search(r'kRuntimeDir\[\] = "([^"]+)"', STARTER.read_text(encoding="utf-8"))
    if match is None:
        raise SystemExit(f"error: cannot read kRuntimeDir from {STARTER}")
    return match.group(1)


def system_library(soname: str) -> pathlib.Path:
    out = subprocess.run(["ldconfig", "-p"], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.strip().split(" => ")
        if len(parts) == 2 and parts[0].split(" ")[0] == soname and "x86-64" in parts[0]:
            return pathlib.Path(parts[1])
    raise SystemExit(f"error: {soname} is not installed (apt install libwebkit2gtk-4.1-0)")


def dependencies(path: pathlib.Path) -> dict[str, pathlib.Path]:
    out = subprocess.run(["ldd", str(path)], check=True, capture_output=True, text=True).stdout
    found: dict[str, pathlib.Path] = {}
    for line in out.splitlines():
        match = re.match(r"\s*(\S+) => (\S+) \(0x", line)
        if match:
            found[match.group(1)] = pathlib.Path(match.group(2))
        elif "not found" in line:
            raise SystemExit(f"error: {path} needs {line.split()[0]}, which is not installed")
    return found


def package_of(path: pathlib.Path) -> str | None:
    for candidate in (path, path.resolve()):
        result = subprocess.run(["dpkg", "-S", str(candidate)], capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.split(":", 1)[0].strip()
        # usrmerge: dpkg knows /lib/x86_64-linux-gnu/... for some libraries
        if str(candidate).startswith("/usr/lib/"):
            result = subprocess.run(["dpkg", "-S", str(candidate)[4:]], capture_output=True, text=True)
            if result.returncode == 0:
                return result.stdout.split(":", 1)[0].strip()
    return None


def copy(source: pathlib.Path, target: pathlib.Path, mode: int) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source.resolve(), target)
    os.chmod(target, mode)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    args = ap.parse_args()

    webkit = system_library(WEBKIT)
    javascriptcore = system_library(JAVASCRIPTCORE)
    # The helpers' folder as WebKit has it written (ldconfig may give the same folder through another path, /lib).
    written = re.search(rb"\0(/[A-Za-z0-9_./-]+/webkit2gtk-4\.1)\0", webkit.read_bytes())
    if written is None:
        raise SystemExit(f"error: no helpers' folder is written in {webkit}: cannot move its helpers")
    helper_dir = pathlib.Path(written.group(1).decode())
    libdir = helper_dir.parent
    target = runtime_dir()
    if len(target) != len(str(helper_dir)):
        raise SystemExit(f"error: kRuntimeDir ({target}) must be as long as {helper_dir}")
    tls = libdir / "gio/modules/libgiognutls.so"
    if not tls.is_file():
        raise SystemExit(f"error: {tls} is missing (apt install glib-networking)")

    out = args.out / "lib/webkit"
    licenses = args.out / "licenses/webkitgtk"
    for folder in (out, licenses):
        if folder.exists():
            shutil.rmtree(folder)

    roots = {WEBKIT: webkit, JAVASCRIPTCORE: javascriptcore}
    programs = {f"webkit2gtk-4.1/{name}": helper_dir / name for name in HELPERS if (helper_dir / name).is_file()}
    programs[f"webkit2gtk-4.1/{INJECTED_BUNDLE}"] = helper_dir / INJECTED_BUNDLE
    programs["gio/libgiognutls.so"] = tls
    libraries: dict[str, pathlib.Path] = dict(roots)
    for path in list(roots.values()) + list(programs.values()):
        for soname, dependency in dependencies(path).items():
            if not SYSTEM.match(soname) and not GTK.match(soname):
                libraries[soname] = dependency

    files: list[pathlib.Path] = []
    for soname, source in sorted(libraries.items()):
        copy(source, out / soname, 0o644)
        files.append(source)
    for name, source in programs.items():
        copy(source, out / name, 0o755 if name.split("/")[-1] in HELPERS else 0o644)
        files.append(source)

    # The helpers' folder, written into WebKit at build time; the injected bundle's path starts with it.
    patched = out / WEBKIT
    data = patched.read_bytes()
    old = str(helper_dir).encode()
    count = data.count(old + b"\0") + data.count(old + b"/")
    if count == 0:
        raise SystemExit(f"error: {helper_dir} is not written in {webkit}: cannot move its helpers")
    patched.write_bytes(data.replace(old, target.encode()))

    # Where every file comes from, with the package's copyright file (the licences: LGPL-2.1 for WebKitGTK).
    packages: dict[str, list[str]] = {}
    for source in files:
        package = package_of(source)
        if package is None:
            raise SystemExit(f"error: no package owns {source}")
        packages.setdefault(package, []).append(source.name)
    licenses.mkdir(parents=True)
    rows = []
    for package in sorted(packages):
        version, source_package = subprocess.run(
            ["dpkg-query", "-W", "-f", "${Version}\t${source:Package}", package],
            check=True, capture_output=True, text=True).stdout.split("\t")
        copyright_file = pathlib.Path("/usr/share/doc") / package / "copyright"
        if not copyright_file.is_file():
            raise SystemExit(f"error: {copyright_file} is missing")
        shutil.copyfile(copyright_file.resolve(), licenses / f"{package}.copyright")
        rows.append(f"{package} {version} (source: {source_package})\n    {', '.join(sorted(packages[package]))}")
    distro = subprocess.run(["lsb_release", "-ds"], capture_output=True, text=True).stdout.strip() or "Ubuntu"
    (licenses / "README.txt").write_text(
        "WebKitGTK and the libraries it needs, bundled for systems that lack them (SteamOS on the Steam Deck).\n"
        f"They are {distro}'s packages, unmodified except one path in libwebkit2gtk-4.1.so.0 (where it finds its\n"
        "helper programs). Each package's copyright file, with its licence, is next to this one. The source of every\n"
        "package is at https://launchpad.net/ubuntu/+source/<source>/<version>, or with apt-get source <package>.\n"
        "These libraries are loaded at run time: any of them in lib/webkit can be replaced by another build.\n\n"
        + "\n".join(rows) + "\n", encoding="utf-8")

    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"{out}: {len(libraries)} libraries, {len(programs)} helpers and modules, {size / 1e6:.1f} MB; "
          f"{len(packages)} packages; helpers at {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
