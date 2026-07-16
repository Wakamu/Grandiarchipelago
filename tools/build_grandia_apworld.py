#!/usr/bin/env python3
"""Build grandia.apworld with the Win32 inject DLL bundled under native/.

Usage (from repo root):
  python tools/build_grandia_apworld.py
  python tools/build_grandia_apworld.py --dll client/build/Release/Grandiarchipelago.dll
  python tools/build_grandia_apworld.py --build-dll
  python tools/build_grandia_apworld.py --out dist/grandia.apworld

Install: copy the .apworld into Archipelago's ``lib/worlds`` or ``custom_worlds`` /
user worlds folder. The Launcher "Grandia Client" extracts/uses
``native/Grandiarchipelago.dll`` from the package for injection.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
WORLD_DIR = REPO_ROOT / "worlds" / "grandia"
NATIVE_DIR = WORLD_DIR / "native"
DEFAULT_DLL = REPO_ROOT / "client" / "build" / "Release" / "Grandiarchipelago.dll"
DEFAULT_OUT = REPO_ROOT / "dist" / "grandia.apworld"

# Match Archipelago APContainer packaging scheme (Files.py); Build APWorlds sets these.
APCONTAINER_VERSION = 7

SKIP_DIR_NAMES = {
    "__pycache__",
    ".git",
    ".pytest_cache",
    "test",
    "tests",
}
SKIP_SUFFIXES = {".pyc", ".pyo", ".dll.old"}


def parse_apignore(world_dir: Path) -> list[str]:
    path = world_dir / ".apignore"
    if not path.is_file():
        return []
    patterns: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        patterns.append(line)
    return patterns


def ignored_by_apignore(rel_posix: str, patterns: list[str]) -> bool:
    """Minimal gitignore-style match for our build (fnmatch on full relative path)."""
    import fnmatch

    ignored = False
    for pattern in patterns:
        negate = pattern.startswith("!")
        pat = pattern[1:] if negate else pattern
        if pat.endswith("/"):
            # directory prefix
            hit = rel_posix.startswith(pat) or fnmatch.fnmatch(rel_posix + "/", pat + "*")
        else:
            hit = fnmatch.fnmatch(rel_posix, pat) or fnmatch.fnmatch(Path(rel_posix).name, pat)
        if hit:
            ignored = not negate
    return ignored


def build_dll() -> Path:
    build_dir = REPO_ROOT / "client" / "build"
    print("Configuring CMake Win32…")
    subprocess.check_call(
        ["cmake", "-S", str(REPO_ROOT / "client"), "-B", str(build_dir), "-A", "Win32"],
    )
    print("Building Release Grandiarchipelago…")
    subprocess.check_call(
        ["cmake", "--build", str(build_dir), "--config", "Release", "--target", "Grandiarchipelago"],
    )
    if not DEFAULT_DLL.is_file():
        raise SystemExit(f"Build finished but DLL missing: {DEFAULT_DLL}")
    return DEFAULT_DLL


def stage_dll(dll_path: Path) -> Path:
    NATIVE_DIR.mkdir(parents=True, exist_ok=True)
    dest = NATIVE_DIR / "Grandiarchipelago.dll"
    shutil.copy2(dll_path, dest)
    print(f"Staged DLL -> {dest} ({dest.stat().st_size} bytes)")
    return dest


def collect_files(world_dir: Path) -> list[tuple[Path, str]]:
    """Return (absolute path, zip arcname under grandia/)."""
    patterns = parse_apignore(world_dir)
    out: list[tuple[Path, str]] = []
    for path in sorted(world_dir.rglob("*")):
        if not path.is_file():
            continue
        if any(part in SKIP_DIR_NAMES for part in path.parts):
            continue
        if path.suffix.lower() in SKIP_SUFFIXES:
            continue
        rel = path.relative_to(world_dir).as_posix()
        if ignored_by_apignore(rel, patterns):
            continue
        out.append((path, f"grandia/{rel}"))
    return out


def write_manifest(world_dir: Path) -> dict:
    manifest_path = world_dir / "archipelago.json"
    if manifest_path.is_file():
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        data = {"game": "Grandia"}
    # Required for packaged .apworld; do not put these in the source-tree JSON
    # permanently — only inject into the zip (recommended workflow).
    packaged = dict(data)
    packaged.setdefault("version", APCONTAINER_VERSION)
    packaged.setdefault("compatible_version", APCONTAINER_VERSION)
    return packaged


def build_apworld(dll_path: Path, out_path: Path) -> Path:
    stage_dll(dll_path)
    files = collect_files(WORLD_DIR)
    dll_rel = "grandia/native/Grandiarchipelago.dll"
    if not any(arc == dll_rel for _, arc in files):
        raise SystemExit("native/Grandiarchipelago.dll was not staged into the world folder")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.exists():
        out_path.unlink()

    manifest = write_manifest(WORLD_DIR)

    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for abs_path, arcname in files:
            if arcname == "grandia/archipelago.json":
                zf.writestr(arcname, json.dumps(manifest, indent=2) + "\n")
            else:
                zf.write(abs_path, arcname)

    # Sanity: zip layout + dll present
    with zipfile.ZipFile(out_path, "r") as zf:
        names = zf.namelist()
        if "grandia/__init__.py" not in names:
            raise SystemExit("Invalid apworld: missing grandia/__init__.py")
        if dll_rel not in names:
            raise SystemExit("Invalid apworld: missing bundled DLL")
        print(f"Packaged {len(names)} files")
        print(f"  DLL in zip: {dll_rel} ({zf.getinfo(dll_rel).file_size} bytes)")

    print(f"Wrote {out_path.resolve()}")
    return out_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Build grandia.apworld with inject DLL")
    parser.add_argument(
        "--dll",
        type=Path,
        default=None,
        help=f"Path to Grandiarchipelago.dll (default: {DEFAULT_DLL})",
    )
    parser.add_argument(
        "--build-dll",
        action="store_true",
        help="Run CMake Win32 Release build before packaging",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"Output .apworld path (default: {DEFAULT_OUT})",
    )
    args = parser.parse_args()

    if not WORLD_DIR.is_dir():
        print(f"World folder missing: {WORLD_DIR}", file=sys.stderr)
        return 1

    dll_path = args.dll
    if args.build_dll:
        dll_path = build_dll()
    elif dll_path is None:
        dll_path = DEFAULT_DLL

    dll_path = dll_path.resolve()
    if not dll_path.is_file():
        print(
            f"DLL not found: {dll_path}\n"
            f"Build it first (cmake -A Win32) or pass --build-dll / --dll",
            file=sys.stderr,
        )
        return 1

    out = args.out
    if out.suffix.lower() != ".apworld":
        out = out.with_suffix(".apworld")
    # Spec: .apworld filenames must be all lowercase
    if out.name != out.name.lower():
        out = out.with_name(out.name.lower())

    build_apworld(dll_path, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
