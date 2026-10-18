#!/usr/bin/env python3
"""Package a private FG3 GPU development artifact; never a release candidate.

No MAKO binaries, Lossless.dll, commercial models or installer mutation.
The full corresponding GFG native/Temporal source is bundled.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import io
import tarfile
import os
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = "GFG-Native-FG"
EXTENSIONS = {".c", ".h", ".inc", ".comp", ".glsl", ".py", ".md", ".json", ".sh", ".txt", ".gd", ".tscn", ".godot"}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def native_elf(binary: Path) -> bytes:
    raw = binary.read_bytes()
    if len(raw) < 64 or raw[:6] != b"\x7fELF\x02\x01" or raw[18:20] != (62).to_bytes(2, "little"):
        raise ValueError("GFG Native needs a Linux x86_64 ELF shared object")
    dynamic = subprocess.run(["readelf", "-d", str(binary)], check=True,
                             capture_output=True, text=True).stdout
    for forbidden in ("libmako", "Lossless.dll", "MAKO-Renderer"):
        if forbidden.lower() in dynamic.lower():
            raise ValueError(f"Prohibited runtime dependency: {forbidden}")
    if "libvulkan" in dynamic.lower():
        # libvulkan is allowed. This path does not require MAKO/LSFG.
        pass
    if b"[gfg-native] GPU bridge active" not in raw:
        raise ValueError("GPU transport is missing; refusing CPU/pass-through artifact")
    if b"rgba_to_rgb" in raw or b"OWN TE8 CPU bridge active" in raw:
        raise ValueError("Legacy CPU pixel transport must not ship in FG3 GPU artifact")
    if b"GPU WSI observer:" in raw:
        raise ValueError("Test-only WSI pixel observer must not ship in the GPU artifact")
    return raw


def source_files() -> dict[str, bytes]:
    selected = {}
    for directory in ("engine/gfg-native", "engine/gfg-temporal"):
        base = ROOT / directory
        for path in base.rglob("*"):
            if not path.is_file() or path.is_symlink():
                continue
            if any(
                part in ("build", "__pycache__", ".git", "results")
                or part == "build-te"
                or part.startswith(("build-", "build_"))
                for part in path.relative_to(base).parts
            ):
                continue
            if path.suffix not in EXTENSIONS and path.name not in ("Makefile",):
                continue
            if path.stat().st_size > 1_000_000:
                raise ValueError(f"Unexpectedly large source file {path}")
            selected["SOURCE/" + path.relative_to(ROOT).as_posix()] = path.read_bytes()
    if not any(name.endswith("src/te_engine.c") for name in selected):
        raise ValueError("Corresponding Temporal Engine source missing")
    if not any(name.endswith("src/gpu_bridge.c") for name in selected):
        raise ValueError("Corresponding Vulkan transport source missing")
    return selected


def worktree_dirty(packaged: dict[str, bytes]) -> bool:
    """Ignore unrelated CI logs while rejecting source changes in the package."""
    included_paths = {
        name.removeprefix("SOURCE/")
        for name in packaged
        if name.startswith("SOURCE/")
    }
    included_paths.update({
        "engine/gfg-native/VkLayer_gfg_native.json",
        "engine/gfg-native/package/install.sh",
        "engine/gfg-native/package/uninstall.sh",
        "engine/gfg-native/package/rollback.sh",
        "engine/gfg-native/package/gfg-native.sh",
        "engine/gfg-native/package/diagnose.sh",
        "engine/gfg-native/package/BUILD-ENVIRONMENT.txt",
        "engine/gfg-native/package/README.ru.md",
        "scripts/accept_report.py",
        "scripts/package_native_fg.py",
        "LICENSE.md",
    })
    diff = subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *sorted(included_paths)], cwd=ROOT)
    if diff.returncode == 1:
        return True
    if diff.returncode != 0:
        raise subprocess.CalledProcessError(diff.returncode, diff.args)
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    ).split(b"\0")
    return any(
        path.decode("utf-8", "surrogateescape") in included_paths
        for path in untracked if path
    )


def package(binary: Path, outdir: Path, version: str, build_environment: str | None = None) -> Path:
    if not re.fullmatch(r"0\.0\.0-fg3-internal\.[0-9]+", version):
        raise ValueError("Only 0.0.0-fg3-internal.N development artifacts are authorized before hardware acceptance")
    files = {
        "libVkLayer_gfg_native.so": native_elf(binary),
        "VkLayer_gfg_native.json": (ROOT / "engine/gfg-native/VkLayer_gfg_native.json").read_bytes(),
        "install.sh": (ROOT / "engine/gfg-native/package/install.sh").read_bytes(),
        "uninstall.sh": (ROOT / "engine/gfg-native/package/uninstall.sh").read_bytes(),
        "rollback.sh": (ROOT / "engine/gfg-native/package/rollback.sh").read_bytes(),
        "gfg-native.sh": (ROOT / "engine/gfg-native/package/gfg-native.sh").read_bytes(),
        "diagnose.sh": (ROOT / "engine/gfg-native/package/diagnose.sh").read_bytes(),
        "accept_report.py": (ROOT / "scripts/accept_report.py").read_bytes(),
        "BUILD-ENVIRONMENT.txt": (ROOT / "engine/gfg-native/package/BUILD-ENVIRONMENT.txt").read_bytes(),
        "README.ru.md": (ROOT / "engine/gfg-native/package/README.ru.md").read_bytes(),
        "LICENSE.md": (ROOT / "LICENSE.md").read_bytes(),
    }
    files.update(source_files())
    files["SOURCE/scripts/build_native_steam_abi.sh"] = (ROOT / "scripts/build_native_steam_abi.sh").read_bytes()
    files["SOURCE/scripts/check_hdr_pq.py"] = (ROOT / "scripts/check_hdr_pq.py").read_bytes()
    for name in ("NATIVE_GSF_IMPLEMENTATION_1_1.md", "STEAM_ABI_BUILD.md", "HARDWARE_ACCEPTANCE_1_1.md", "HDR_PQ_RECONSTRUCTION.md", "DEFERRED_G2.md"):
        files["SOURCE/docs/fg3/"+name]=(ROOT / "docs/fg3" / name).read_bytes()
    manifest = json.loads(files["VkLayer_gfg_native.json"])
    if manifest["layer"]["name"] != "VK_LAYER_GFG_native" or \
       manifest["layer"]["library_path"] != "./libVkLayer_gfg_native.so":
        raise ValueError("Native manifest identity mismatch")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = worktree_dirty(files)
    provenance = {
        "schema": 1,
        "version": version,
        "source_commit": None if dirty else commit,
        "source_base_commit": commit,
        "source_worktree_dirty": dirty,
        "source_identity": "SHA256SUMS.txt covers included corresponding source",
        "algorithm": "Independent GFG GSF adaptive GPU analysis; x2 t=1/2; x3 t=1/3,2/3",
        "transport": "bounded asynchronous WSI output; serialized application queue; GPU image copies; no CPU pixel staging",
        "multipliers": [2, 3],
        "default_multiplier": 3,
        "architecture": "linux-x86_64",
        "runtime_requires": ["Vulkan 1.2", "64-bit Linux"],
        "elf_glibc_versions": sorted(set(re.findall(r"GLIBC_[0-9.]+", subprocess.check_output(["readelf", "--version-info", str(binary)], text=True)))),
        "elf_needed": re.findall(r"Shared library: \[(.*?)\]", subprocess.check_output(["readelf", "-d", str(binary)], text=True)),
        "runtime_forbidden": ["MAKO Renderer", "Lossless Scaling", "Lossless.dll"],
        "build_environment": build_environment or (
            "GitHub Actions ubuntu-22.04, gcc-12, Vulkan-Headers v1.4.365; see BUILD-ENVIRONMENT.txt"
            if os.environ.get("GITHUB_ACTIONS") == "true"
            else "Local build; environment not attested. BUILD-ENVIRONMENT.txt describes the canonical CI rebuild."
        ),
        "stage": "experimental; hardware acceptance NOT TESTED",
        "hardware_acceptance": False,
        "physical_display_verified": False,
        "elf_sha256": digest(files["libVkLayer_gfg_native.so"]),
    }
    files["MANIFEST.json"] = (json.dumps(provenance, sort_keys=True, indent=2) + "\n").encode()
    checksum = "".join(f"{digest(data)}  {name}\n" for name, data in sorted(files.items()))
    files["SHA256SUMS.txt"] = checksum.encode()

    outdir.mkdir(parents=True, exist_ok=True)
    path = outdir / ("GFG-Native-FG-v" + version.replace(".", "_") + ".zip")
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6, strict_timestamps=True) as archive:
        for name, data in sorted(files.items()):
            zi = zipfile.ZipInfo(f"{ARTIFACT_ROOT}/{name}", date_time=(2026, 10, 15, 0, 0, 0))
            zi.create_system = 3
            zi.external_attr = (0o100755 if name.endswith((".sh", ".so")) else 0o100644) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(zi, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    with zipfile.ZipFile(path) as z:
        bad = z.testzip()
        if bad:
            raise ValueError(f"ZIP CRC failure: {bad}")
    (outdir / "SHA256SUMS.txt").write_text(f"{digest(path.read_bytes())}  {path.name}\n")
    print(f"Native FG test package: {path} sha256={digest(path.read_bytes())}")
    return path


def decky_package(native_zip: Path, index: Path, outdir: Path) -> Path:
    """Bundle the integrated GFG UI/backend and the same verified component.

    Native mode remains opt-in; the old optional renderer package is preserved
    for compatibility and is never selected by the native launcher.
    """
    subprocess.run(["git", "diff", "--quiet", "HEAD", "--", "py_modules", "frontend", "dist", "package.json", "plugin.json"], cwd=ROOT, check=True)
    if index.read_bytes() != (ROOT / "dist/index.js").read_bytes():
        raise ValueError("Decky frontend must match the committed dist/index.js")
    files = {}
    modes = {}
    data = subprocess.check_output(["git", "archive", "HEAD"], cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive:
            if not member.isfile() or member.name.startswith((".github/", "docs/", "tests/")):
                continue
            stream = archive.extractfile(member)
            files[member.name] = stream.read()
            modes[member.name] = member.mode & 0o777
    files["dist/index.js"] = index.read_bytes()
    with zipfile.ZipFile(native_zip) as archive:
        for item in archive.infolist():
            name = item.filename.removeprefix(ARTIFACT_ROOT + "/")
            if item.is_dir() or name == item.filename or name.startswith("/") or ".." in Path(name).parts:
                raise ValueError("Invalid component archive member")
            files["bin/gfg-native/" + name] = archive.read(item)
            modes["bin/gfg-native/" + name] = (item.external_attr >> 16) & 0o777
    metadata = json.loads(files["package.json"])
    component = json.loads(files["bin/gfg-native/MANIFEST.json"])
    metadata["base_version"] = metadata["version"]
    sequence = component["version"].rsplit(".", 1)[1]
    metadata["version"] += "-gsf." + sequence
    metadata["gfgPrerelease"] = True
    metadata["bundled_native_gsf"] = component
    files["package.json"] = (json.dumps(metadata, indent=2) + "\n").encode()
    lock = json.loads(files["package-lock.json"])
    lock["version"] = lock["packages"][""]["version"] = metadata["version"]
    files["package-lock.json"] = (json.dumps(lock, indent=2) + "\n").encode()
    # Keep the installed UI, backend and diagnostic build identity consistent.
    # The archive's base_version + component source_commit record this explicit
    # packaging transform; the source checkout keeps upstream's base version.
    for name, prefix in (("governor_service.py", "VERSION"), ("log_report.py", "CURRENT_VERSION")):
        path = "py_modules/gfg_plugin/" + name
        files[path] = re.sub(r'^' + prefix + r' = "[^"]+"',
                            prefix + ' = "' + metadata["version"] + '"',
                            files[path].decode(), count=1, flags=re.M).encode()
    for name in ("plugin.py", "session_recorder.py"):
        path = "py_modules/gfg_plugin/" + name
        files[path] = files[path].replace(('GFG Extreme ' + metadata["base_version"]).encode(),
                                        ('GFG Extreme ' + metadata["version"]).encode())
    files["NATIVE-GSF-EXPERIMENTAL.txt"] = b"Native GSF hardware acceptance: NOT TESTED. Bundled independent component is verified and installed at plugin startup, without enabling frame generation. Settings > Diagnostics > GFG Native Generator: Off / x2 / x3. The Steam launch option is shown only when the executable launcher and component are present. Off by default. No stable release.\n"
    path = outdir / ("GFG-Extreme-" + metadata["version"] + ".zip")
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            zi = zipfile.ZipInfo("GFG-Extreme/" + name, date_time=(2026, 10, 16, 0, 0, 0))
            zi.create_system = 3
            mode = modes.get(name, 0o755 if name.endswith((".sh", ".so")) else 0o644)
            zi.external_attr = (0o100000 | mode) << 16
            archive.writestr(zi, data, compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(path) as archive:
        if archive.testzip():
            raise ValueError("Decky archive CRC failure")
    with (outdir / "SHA256SUMS.txt").open("a") as sums:
        sums.write(f"{digest(path.read_bytes())}  {path.name}\n")
    print(f"Integrated experimental Decky package: {path} sha256={digest(path.read_bytes())}")
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--version", default="0.0.0-fg3-internal.10")
    parser.add_argument("--build-environment", help="Actual build environment; otherwise inferred from GitHub Actions or marked unattested")
    parser.add_argument("--decky-index", type=Path, help="Also build integrated Decky experiment with this freshly built frontend")
    args = parser.parse_args()
    native = package(args.binary.resolve(), args.outdir.resolve(), args.version, args.build_environment)
    if args.decky_index:
        decky_package(native, args.decky_index.resolve(), args.outdir.resolve())
