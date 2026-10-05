#!/usr/bin/env python3
# Windows port modifications by yaonikaixin999999, 2026-10-05.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install and verify the pinned MIT-licensed FSR 4 v07 INT8 Vulkan assets.

This is the Windows equivalent of fetch_fsr4_assets.sh / fsr4_optimize.sh.
No GPU work is performed. Optimizer rewrites match the repository's Perl
scripts, including their signed INT8 correction and pass 11 bounds guard.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
import urllib.request

COMMIT = "ae8d628fae208813172446d1e49ed94150b04658"
BASE = f"https://raw.githubusercontent.com/FireBurn/Q2RTX/{COMMIT}/baseq2/fsr4_shaders"
MODELS = ("native", "quality", "balanced", "performance", "ultraperf", "drs")
TIERS = (1080, 2160)
ROOT = Path(__file__).resolve().parents[1]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_payload(name: str, data: bytes, expected: dict | None = None) -> None:
    if not data:
        raise ValueError(f"Empty asset: {name}")
    if name.endswith(".spv"):
        if len(data) < 20 or len(data) % 4 or struct.unpack_from("<I", data)[0] != 0x07230203:
            raise ValueError(f"Invalid SPIR-V header: {name}")
    if expected and (len(data) != expected["size"] or digest(data) != expected["sha256"]):
        raise ValueError(f"Upstream manifest size/hash mismatch: {name}")


def download(name: str, dest: Path, expected: dict | None = None, refresh: bool = False) -> bool:
    path = dest / name
    if not refresh and path.is_file():
        try:
            verify_payload(name, path.read_bytes(), expected)
            return False
        except ValueError:
            pass
    for attempt in range(4):
        try:
            request = urllib.request.Request(f"{BASE}/{name}", headers={"User-Agent": "Bloodborne-Windows-FSR4-assets"})
            with urllib.request.urlopen(request, timeout=45) as response:
                data = response.read()
            verify_payload(name, data, expected)
            part = path.with_name(path.name + ".part")
            part.write_bytes(data)
            part.replace(path)
            return True
        except Exception:
            if attempt == 3:
                raise
            time.sleep(attempt + 1)
    raise AssertionError("unreachable")


def signed_unpack(source: str) -> str:
    source, count = re.subn(r"\bunpack8\(", "bbUnpackS8(", source)
    if not count:
        raise ValueError("Expected signed INT8 unpacks")
    helper = """// bbport: signed int8 unpack (spirv-cross emits the unsigned unpack8(uint)).
i8vec4 bbUnpackS8(uint v)
{
    return unpack8(int(v));
}

"""
    source, count = re.subn(r"^(void main\(\)\n)", lambda m: helper + m[1], source, flags=re.M)
    if count != 1:
        raise ValueError("Missing or duplicate main")
    return source


def rewrite_post(source: str) -> str:
    if "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;" not in source:
        raise ValueError("Unexpected post pass local size")
    source = signed_unpack(source)
    for image, array in sorted((("rw_recurrent_0", "bb_rec"), ("rw_history_color", "bb_hist"), ("rw_mlsr_output_color", "bb_out"))):
        pattern = rf"imageStore\({image}, ivec2\((_\d+)\), "
        source, count = re.subn(pattern, lambda m: f"{array}[bbLocal({m[1]})] = vec4(", source)
        if count != 1:
            raise ValueError(f"Expected one store to {image}, found {count}")
    decl = """// bbport: the workgroup's 16x16 output block, stored in contiguous rows after the loop.
shared vec4 bb_rec[256];
shared vec4 bb_hist[256];
shared vec4 bb_out[256];
uint bbLocal(uvec2 p)
{
    uvec2 l = p - gl_WorkGroupID.xy * 16u;
    return l.y * 16u + l.x;
}

void main()
"""
    source, count = re.subn(r"^void main\(\)\n", lambda _: decl, source, flags=re.M)
    if count != 1:
        raise ValueError("Missing post main")
    flush = """    barrier();
    uvec2 bbBase = gl_WorkGroupID.xy * 16u;
    for (uint bbK = 0u; bbK < 4u; bbK++)
    {
        uint bbI = gl_LocalInvocationIndex + 64u * bbK;
        ivec2 bbP = ivec2(bbBase + uvec2(bbI % 16u, bbI / 16u));
        imageStore(rw_recurrent_0, bbP, bb_rec[bbI]);
        imageStore(rw_history_color, bbP, bb_hist[bbI]);
        imageStore(rw_mlsr_output_color, bbP, bb_out[bbI]);
    }
}
"""
    source, count = re.subn(r"\n}\s*\Z", lambda _: "\n" + flush, source)
    if count != 1:
        raise ValueError("Missing end of post main")
    return source


def rewrite_pass11(source: str) -> str:
    bounds = re.search(r"lessThan\(uvec3\(_\d+\), uvec3\((\d+)u, (\d+)u, 32u\)\)", source)
    if not bounds:
        raise ValueError("No pass 11 input bounds check")
    if re.search(r"\bbarrier\(|\bshared\b", source):
        raise ValueError("Pass 11 unexpectedly uses shared memory/barriers")
    source = signed_unpack(source)
    guard = f"""void main()
{{
    // bbport: invocations past the input width would write the next row's first pixels.
    if (gl_GlobalInvocationID.x >= {bounds[1]}u || gl_GlobalInvocationID.y >= {bounds[2]}u)
    {{
        return;
    }}
"""
    source, count = re.subn(r"^void main\(\)\n{\n", lambda _: guard, source, flags=re.M)
    if count != 1:
        raise ValueError("Missing pass 11 main")
    return source


def run_tool(args: list[str]) -> str:
    result = subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.stdout


def optimize(dest: Path, tool_dir: Path | None) -> list[str]:
    def tool(name: str) -> str:
        candidate = tool_dir / (name + ".exe") if tool_dir else None
        found = str(candidate) if candidate and candidate.is_file() else shutil.which(name)
        if not found:
            raise FileNotFoundError(f"Missing {name}; use --tools-dir or --no-optimize")
        return found

    cross, compiler, validator = (tool(name) for name in ("spirv-cross", "glslangValidator", "spirv-val"))
    target = dest / "opt"
    target.mkdir(exist_ok=True)
    outputs = []
    with tempfile.TemporaryDirectory(prefix="bb-fsr4-") as scratch:
        scratch = Path(scratch)
        for original in sorted([*dest.glob("*_post.spv"), *dest.glob("*_pass11.spv")]):
            is_post = original.name.endswith("_post.spv")
            entry = "main" if is_post else "fsr4_model_v07_i8_pass11"
            run_tool([cross, str(original), "--vulkan-semantics", "--entry", entry, "--output", str(scratch / "in.comp")])
            source = (scratch / "in.comp").read_text(encoding="utf-8")
            source = rewrite_post(source) if is_post else rewrite_pass11(source)
            (scratch / "out.comp").write_text(source, encoding="utf-8", newline="\n")
            run_tool([compiler, "-V", "--target-env", "vulkan1.3", "-S", "comp", "-e", entry, "--source-entrypoint", "main", str(scratch / "out.comp"), "-o", str(scratch / "out.spv")])
            run_tool([validator, "--target-env", "vulkan1.3", str(scratch / "out.spv")])
            payload = (scratch / "out.spv").read_bytes()
            verify_payload(original.name, payload)
            part = target / (original.name + ".part")
            part.write_bytes(payload)
            part.replace(target / original.name)
            outputs.append("opt/" + original.name)
            print(f"Optimized and validated {original.name}", flush=True)
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=ROOT / "fsr4_shaders")
    parser.add_argument("--tools-dir", type=Path, default=ROOT.parent / "tools-local/msys64/ucrt64/bin")
    parser.add_argument("--no-optimize", action="store_true")
    args = parser.parse_args()
    args.dest.mkdir(parents=True, exist_ok=True)
    download("LICENSE-FSR4-v07.txt", args.dest, refresh=True)
    expected = {}
    manifests = []
    for model in MODELS:
        name = f"fsr4_model_v07_i8_{model}_shader_manifest.json"
        download(name, args.dest, refresh=True)
        manifest = json.loads((args.dest / name).read_text(encoding="utf-8"))
        if manifest["schema_version"] != 1 or manifest["model"] != f"fsr4_model_v07_i8_{model}":
            raise ValueError(f"Unexpected manifest schema/model: {name}")
        manifests.append(name)
        for artifact, metadata in manifest["artifacts"].items():
            if "/" in artifact or "\\" in artifact or artifact in (".", ".."):
                raise ValueError(f"Unsafe manifest filename: {artifact}")
            if "_4320_" in artifact:
                continue
            if artifact in expected and expected[artifact] != metadata:
                raise ValueError(f"Conflicting manifest entry: {artifact}")
            expected[artifact] = metadata
    downloaded = 0
    with ThreadPoolExecutor(max_workers=4) as workers:
        futures = {workers.submit(download, name, args.dest, metadata): name for name, metadata in expected.items()}
        for index, future in enumerate(as_completed(futures), 1):
            downloaded += bool(future.result())
            if index % 20 == 0 or index == len(futures):
                print(f"Verified {index}/{len(futures)} model/shader assets", flush=True)
    validated = []
    validator = args.tools_dir / "spirv-val.exe"
    if validator.is_file():
        for name in sorted(expected):
            if name.endswith(".spv"):
                run_tool([str(validator), "--target-env", "vulkan1.3", str(args.dest / name)])
                validated.append(name)
    optimized = [] if args.no_optimize else optimize(args.dest, args.tools_dir)
    files = ["LICENSE-FSR4-v07.txt", *manifests, *sorted(expected), *optimized]
    report = {
        "source": BASE, "pinned_commit": COMMIT, "license": "LICENSE-FSR4-v07.txt (MIT)",
        "tiers": list(TIERS), "models": list(MODELS), "downloaded": downloaded,
        "upstream_manifest_verified": len(expected), "spirv_validated": len(validated) + len(optimized),
        "optimizer": "Python translation of fsr4_post_lds.pl, fsr4_pass11_guard.pl and Fsr4SpirvCrossFixes.pm",
        "gpu_validation_performed": False,
        "files": {name: {"size": (args.dest / name).stat().st_size, "sha256": digest((args.dest / name).read_bytes())} for name in files},
    }
    (args.dest / "ASSET_INSTALL_REPORT.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"FSR 4 assets ready: {len(files)} files; {len(optimized)} optimized; {len(validated) + len(optimized)} SPIR-V validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
