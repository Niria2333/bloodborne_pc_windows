#!/usr/bin/env python3
# Windows port modifications by yaonikaixin999999, 2026-10-05.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package the native Windows build with its recursive PE DLL dependencies.

Windows API sets and DLLs present in System32 are host dependencies. Other
imports must resolve in the configured MSYS2 UCRT64 bin directory. No game
assets, driver ICDs, or arbitrary PATH directories are searched or copied.
"""
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def refresh_manifest(destination: Path) -> None:
    files = sorted((p for p in destination.rglob("*") if p.is_file()
                    and p.name != "SHA256SUMS.txt"), key=lambda p: p.relative_to(destination).as_posix())
    (destination / "SHA256SUMS.txt").write_text(
        "".join(f"{sha256(path)}  {path.relative_to(destination).as_posix()}\n" for path in files),
        encoding="utf-8")


def parse_pacman_fields(path: Path) -> dict[str, list[str]]:
    fields: dict[str, list[str]] = {}
    field = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("%") and line.endswith("%"):
            field = line.strip("%")
            fields[field] = []
        elif line and field:
            fields[field].append(line)
    return fields


def package_database(prefix: Path) -> dict[str, tuple[Path, dict[str, list[str]]]]:
    database = prefix.parent / "var/lib/pacman/local"
    result = {}
    if not database.is_dir():
        raise RuntimeError(f"MSYS2 package ownership database not found: {database}")
    for entry in database.iterdir():
        if not (entry / "desc").is_file() or not (entry / "files").is_file():
            continue
        metadata = parse_pacman_fields(entry / "desc")
        files = parse_pacman_fields(entry / "files").get("FILES", [])
        for name in files:
            if name.lower().startswith(prefix.name.lower() + "/bin/") and name.lower().endswith(".dll"):
                result[Path(name).name.lower()] = (entry, metadata)
    return result


def pe_imports(objdump: Path, binary: Path, environment: dict[str, str]) -> list[str]:
    result = subprocess.run([str(objdump), "-p", str(binary)], env=environment,
                            capture_output=True, text=True, check=True)
    if "file format pei-x86-64" not in result.stdout:
        raise RuntimeError(f"Expected Windows x64 PE binary: {binary}")
    return sorted(set(re.findall(r"DLL Name:\s*(\S+)", result.stdout)), key=str.lower)


def find_dependencies(executable: Path, prefix: Path, system32: Path, objdump: Path):
    available = {path.name.lower(): path for path in (prefix / "bin").iterdir() if path.is_file()}
    system = {path.name.lower(): path for path in system32.iterdir() if path.is_file()}
    environment = dict(os.environ)
    environment["PATH"] = os.pathsep.join([str(prefix / "bin"), str(system32)])
    queue = deque([executable])
    binaries = {executable.name.lower(): executable}
    graph: dict[str, list[dict[str, str]]] = {}
    while queue:
        binary = queue.popleft()
        imports = []
        for name in pe_imports(objdump, binary, environment):
            key = name.lower()
            if key.startswith(("api-ms-win-", "ext-ms-win-")):
                imports.append({"name": name, "kind": "windows-api-set"})
            elif key in available:
                imported = available[key]
                imports.append({"name": name, "kind": "bundled", "source": f"ucrt64/bin/{imported.name}"})
                if key not in binaries:
                    binaries[key] = imported
                    queue.append(imported)
            elif key in system:
                imports.append({"name": name, "kind": "windows-system32", "source": f"System32/{system[key].name}"})
            else:
                raise RuntimeError(f"Unresolved non-system import {name!r} from {binary}")
        graph[binary.name] = imports
    return binaries, graph


def copy_notices(destination: Path, prefix: Path, binaries: dict[str, Path], executable: Path,
                 source_url: str | None = None) -> list[dict]:
    notices = destination / "notices"
    notices.mkdir(parents=True, exist_ok=True)
    # Include notices for the static source components used by the renderer.
    source_notices = [ROOT / "LICENSE", ROOT / "gpu/shadps4/LICENSE",
                      ROOT / "third_party/LibAtrac9/LICENSE", ROOT / "gpu/third_party/imgui/LICENSE.txt",
                      ROOT / "gpu/third_party/half/LICENSE.txt", ROOT / "gpu/third_party/fonts/LICENSE-DejaVu.txt",
                      ROOT / "gpu/third_party/sirit/LICENSE.txt", ROOT / "gpu/third_party/sirit/externals/SPIRV-Headers/LICENSE",
                      ROOT / "gpu/third_party/fsr-vulkan/LICENSE.txt", ROOT / "gpu/third_party/fsr-vulkan/NOTICE.md",
                      ROOT / "gpu/third_party/fsr-vulkan/upstream/ffx-1.1.4/sdk/LICENSE.txt"]
    cache = ROOT / "out/windows/CMakeCache.txt"
    if cache.is_file():
        for source in re.findall(r"^FETCHCONTENT_SOURCE_DIR_\w+:PATH=(.+)$", cache.read_text(), re.MULTILINE):
            source_path = Path(source.strip())
            source_notices.extend(p for p in source_path.iterdir() if p.is_file()
                                  and re.match(r"(?i)^(license|copying|notice)(\.|$)", p.name))
    for path in source_notices:
        if not path.is_file():
            raise RuntimeError(f"Required source license notice is missing: {path}")
        relative = path.relative_to(ROOT)
        target = notices / "source" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)

    database = package_database(prefix)
    package_records: dict[str, dict] = {}
    for key, binary in binaries.items():
        if binary == executable:
            continue
        if key not in database:
            raise RuntimeError(f"No MSYS2 package ownership metadata for {binary}")
        entry, fields = database[key]
        name = fields["NAME"][0]
        if name in package_records:
            package_records[name]["dlls"].append(binary.name)
            continue
        package_notice = notices / "msys2" / name
        package_notice.mkdir(parents=True, exist_ok=True)
        shutil.copy2(entry / "desc", package_notice / "package-metadata.txt")
        copied = []
        for record in parse_pacman_fields(entry / "files").get("FILES", []):
            if "/share/licenses/" in record and not record.endswith("/"):
                source = prefix.parent / record
                if not source.is_file():
                    raise RuntimeError(f"Installed license notice is missing: {source}")
                target = package_notice / Path(record).relative_to(prefix.name + "/share/licenses")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                copied.append(target.relative_to(destination).as_posix())
        short_name = name.removeprefix("mingw-w64-ucrt-x86_64-")
        base = fields.get("BASE", ["mingw-w64-" + short_name])[0]
        package_records[name] = {
            "name": name, "version": fields["VERSION"][0], "license": fields.get("LICENSE", []),
            "upstream": fields.get("URL", []), "dlls": [binary.name], "license_files": copied,
            "build_recipe": f"https://github.com/msys2/MINGW-packages/tree/master/{base}",
            "package_page": f"https://packages.msys2.org/packages/{name}",
        }

    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True,
                            text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout.strip())
    if not source_url:
        for remote in ("windows-origin", "origin"):
            result = subprocess.run(["git", "remote", "get-url", remote], cwd=ROOT,
                                    capture_output=True, text=True)
            if result.returncode == 0:
                source_url = result.stdout.strip()
                break
    source_url = source_url or "Source archive accompanying this build"
    if source_url.startswith("git@github.com:"):
        source_url = "https://github.com/" + source_url.split(":", 1)[1]
    source_url = source_url.removesuffix(".git").rstrip("/")
    (destination / "SOURCE-NOTICES.txt").write_text(
        f"Experimental native Windows build of bbport\n\n"
        f"Upstream source: https://github.com/deadinside28/bloodborne_pc\n"
        "Windows port base commit: 5224a6d137d8c4efaab69f2412f4bb86227b9ab8\n"
        f"Corresponding source repository: {source_url}\n"
        f"Source checkout commit: {commit}\n"
        f"Source checkout has uncommitted changes: {'yes' if dirty else 'no'}\n"
        "For a modified checkout, include the exact modified source archive before distributing binaries.\n"
        "Build instructions: docs/WINDOWS.md; validation scope: docs/WINDOWS_VALIDATION.md.\n"
        "The project and shadPS4 license texts are in notices/source. Static third-party components\n"
        "retain their included license notices there. DLL ownership, exact versions, upstream source URLs,\n"
        "MSYS2 build recipes, and SPDX license identifiers are recorded in dependencies.json.\n"
        "Installed per-package license files and metadata are in notices/msys2. Some MSYS2 packages\n"
        "declare standard SPDX licenses without installing a separate text; their identifiers are retained.\n"
        "No Bloodborne game files, Sony system modules, firmware, saves, or Vulkan driver ICDs are included.\n"
        "The installed Windows system and GPU driver provide OS DLLs/API sets and the Vulkan ICD.\n"
        "Windows game entry and short 1080p gameplay have been observed; sustained 4K/60 FPS\n"
        "and a full playthrough have not been verified. FSR 4.1.1 Windows adaptation is pending.\n",
        encoding="utf-8")
    return sorted(package_records.values(), key=lambda record: record["name"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "out/windows/bin/bb-probe.exe")
    parser.add_argument("--prefix", type=Path, default=ROOT.parent / "tools-local/msys64/ucrt64")
    parser.add_argument("--destination", type=Path, default=ROOT / "dist/windows")
    parser.add_argument("--system32", type=Path,
                        default=Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32")
    parser.add_argument("--refresh-manifest", action="store_true", help="Hash current package files only")
    parser.add_argument("--source-url", help="Public repository containing this build's corresponding source")
    args = parser.parse_args()
    destination = args.destination.resolve()
    if args.refresh_manifest:
        if not destination.is_dir():
            raise RuntimeError(f"Package directory does not exist: {destination}")
        refresh_manifest(destination)
        print(f"Refreshed {destination / 'SHA256SUMS.txt'}")
        return 0
    executable, prefix, system32 = args.executable.resolve(), args.prefix.resolve(), args.system32.resolve()
    objdump = prefix / "bin/objdump.exe"
    if not executable.is_file() or not objdump.is_file():
        raise RuntimeError(f"Missing executable or objdump: {executable}, {objdump}")
    binaries, graph = find_dependencies(executable, prefix, system32, objdump)
    destination.mkdir(parents=True, exist_ok=True)
    old_manifest = destination / "dependencies.json"
    old_binaries = json.loads(old_manifest.read_text()).get("binaries", []) if old_manifest.is_file() else []
    for old in old_binaries:
        name = old["name"]
        if Path(name).name != name:
            raise RuntimeError(f"Unsafe previous manifest file name: {name}")
        if name.lower() not in binaries:
            (destination / name).unlink(missing_ok=True)
    for binary in binaries.values():
        shutil.copy2(binary, destination / binary.name)
    packages = copy_notices(destination, prefix, binaries, executable, args.source_url)
    binary_records = [{"name": path.name, "source": path.name if path == executable else f"ucrt64/bin/{path.name}", "size": path.stat().st_size,
                       "sha256": sha256(path)} for path in sorted(binaries.values(), key=lambda p: p.name.lower())]
    write_json(destination / "dependencies.json", {"format_version": 1, "architecture": "x86_64",
               "executable": executable.name, "binaries": binary_records, "imports": graph, "packages": packages})
    refresh_manifest(destination)
    total = sum(path.stat().st_size for path in destination.rglob("*") if path.is_file())
    print(f"Packaged {len(binaries)-1} DLLs from {len(packages)} MSYS2 packages; no unresolved imports.")
    print(f"Destination: {destination}")
    print(f"Total size: {total:,} bytes ({total/1024/1024:.2f} MiB)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
