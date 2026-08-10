#!/usr/bin/env python3
"""Stage Redux content overlays that differ from vanilla into native/redux_content.

Mirrors Redux `content/` layout so the DLL fopen hook can redirect by relative path:

  FIELD/*.MDP, SHOP.BIN, WINDT.BIN, …
  BIN/MCHAR.DAT
  BATLE/M_DAT.BIN, *.BBG
  TEXT/EN/*.SCN, TEXT1.BIN, strings.txt

Writes manifest.json with relative posix paths for apworld extract.

Usage (from repo root):
  python tools/stage_redux_field_overlays.py
  python tools/stage_redux_field_overlays.py --dest client/build/Release/redux_content
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
REDUX_CONTENT = (
    REPO
    / "data"
    / "redux_spike"
    / "redux_0.4.9"
    / "GrandiaRemasteredRedux_V0.4.9"
    / "content"
)
DEFAULT_VANILLA = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\GRANDIA HD Remaster\content"
)
DEFAULT_DEST = REPO / "worlds" / "grandia" / "native" / "redux_content"

# Rel paths that must be present after staging.
REQUIRED = (
    "FIELD/SHOP.BIN",
    "FIELD/204C.MDP",
    "FIELD/WINDT.BIN",
    "BIN/MCHAR.DAT",
    "BATLE/M_DAT.BIN",
    "TEXT/EN/TEXT1.BIN",
)

# Always include when present in Redux (even if identical — cheap + safe).
ALWAYS = (
    "FIELD/SHOP.BIN",
    "FIELD/ITEM.BIN",
    "FIELD/FWIN.BIN",
    "FIELD/WINDT.BIN",
    "BIN/MCHAR.DAT",
    "BATLE/M_DAT.BIN",
    "TEXT/EN/TEXT1.BIN",
    "TEXT/EN/strings.txt",
)

# (subdir under content, glob patterns) — only files that differ (or ALWAYS).
SCAN_SPECS: list[tuple[str, list[str]]] = [
    ("FIELD", ["*.MDP", "*.mdp", "SHOP.BIN", "ITEM.BIN", "FWIN.BIN", "WINDT.BIN"]),
    ("BIN", ["MCHAR.DAT", "mchar.dat"]),
    ("BATLE", ["*.BBG", "*.bbg", "M_DAT.BIN", "m_dat.bin"]),
    ("TEXT/EN", ["*.SCN", "*.scn", "TEXT1.BIN", "text1.bin", "strings.txt", "STRINGS.TXT"]),
]


def resolve_vanilla(path: Path | None) -> Path | None:
    if path and path.is_dir():
        return path
    if DEFAULT_VANILLA.is_dir():
        return DEFAULT_VANILLA
    return None


def vanilla_counterpart(vanilla_root: Path | None, rel: Path) -> Path | None:
    if vanilla_root is None:
        return None
    q = vanilla_root / rel
    if q.is_file():
        return q
    # Locale / case fallbacks for TEXT.
    parts = list(rel.parts)
    if len(parts) >= 2 and parts[0].upper() == "TEXT":
        for lang in (parts[1].upper(), parts[1].lower(), "EN", "en"):
            alt = vanilla_root / "TEXT" / lang / parts[-1]
            if alt.is_file():
                return alt
            alt = vanilla_root / "TEXT" / lang / parts[-1].lower()
            if alt.is_file():
                return alt
    lower = vanilla_root / Path(*[p.lower() for p in rel.parts])
    if lower.is_file():
        return lower
    return None


def canonicalize_rel(rel: str) -> str:
    """Stable posix rel path: keep TEXT/EN lang casing, uppercase filename."""
    parts = Path(rel.replace("\\", "/")).parts
    if not parts:
        return rel.replace("\\", "/")
    dirs = list(parts[:-1])
    name = parts[-1].upper()
    # Canonical directory casing used by Redux / the game.
    if dirs and dirs[0].upper() == "FIELD":
        dirs[0] = "FIELD"
    elif dirs and dirs[0].upper() == "BIN":
        dirs[0] = "BIN"
    elif dirs and dirs[0].upper() == "BATLE":
        dirs[0] = "BATLE"
    elif len(dirs) >= 2 and dirs[0].upper() == "TEXT":
        dirs[0] = "TEXT"
        dirs[1] = dirs[1].upper()  # EN, JA, …
    return "/".join([*dirs, name])


def collect_changed(redux: Path, vanilla: Path | None, all_files: bool) -> list[str]:
    """Return sorted relative posix paths to stage."""
    names: set[str] = set()

    for always in ALWAYS:
        if (redux / always).is_file():
            names.add(canonicalize_rel(always))

    for sub, patterns in SCAN_SPECS:
        src_dir = redux / sub
        if not src_dir.is_dir():
            continue
        seen_in_dir: set[str] = set()
        for pat in patterns:
            for p in src_dir.glob(pat):
                if not p.is_file():
                    continue
                # Dedupe case-insensitive double hits from *.MDP + *.mdp globs.
                key = p.name.upper()
                if key in seen_in_dir:
                    continue
                seen_in_dir.add(key)
                rel = canonicalize_rel(p.relative_to(redux).as_posix())
                if all_files or vanilla is None:
                    names.add(rel)
                    continue
                q = vanilla_counterpart(vanilla, Path(rel))
                if q is None or q.read_bytes() != p.read_bytes():
                    names.add(rel)

    return sorted(names, key=str.upper)


def stage(dest_root: Path, rels: list[str], redux: Path) -> Path:
    missing_required = [r for r in REQUIRED if not (redux / r).is_file()]
    if missing_required:
        raise SystemExit(f"Redux content missing required: {missing_required}")

    total = 0
    staged: list[str] = []
    by_dir: dict[str, int] = {}

    for rel in rels:
        src = redux / rel
        if not src.is_file():
            print(f"skip missing {rel}", file=sys.stderr)
            continue
        dest = dest_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        total += dest.stat().st_size
        staged.append(rel.replace("\\", "/"))
        top = rel.split("/", 1)[0]
        by_dir[top] = by_dir.get(top, 0) + 1

    for req in REQUIRED:
        if req not in staged:
            raise SystemExit(f"Failed to stage required overlay: {req}")

    manifest = {
        "version": 2,
        "source": "GrandiaRemasteredRedux_V0.4.9",
        "files": staged,
        "bytes": total,
        "by_dir": by_dir,
    }
    man_path = dest_root / "manifest.json"
    man_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"Staged {len(staged)} overlays -> {dest_root} "
        f"({total / (1024 * 1024):.1f} MiB) {by_dir}"
    )
    print(f"Wrote {man_path}")
    return man_path


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage Redux content overlays for fopen redirect")
    ap.add_argument(
        "--dest",
        type=Path,
        default=DEFAULT_DEST,
        help=f"Overlay root (default: {DEFAULT_DEST})",
    )
    ap.add_argument(
        "--vanilla",
        type=Path,
        default=None,
        help="Vanilla content/ for diff filter (default: Steam install)",
    )
    ap.add_argument(
        "--all",
        "--all-mdp",
        dest="all_files",
        action="store_true",
        help="Copy every scanned Redux file (ignore vanilla diff)",
    )
    args = ap.parse_args()

    if not REDUX_CONTENT.is_dir():
        print(f"Redux content missing: {REDUX_CONTENT}", file=sys.stderr)
        return 1

    vanilla = None if args.all_files else resolve_vanilla(args.vanilla)
    if vanilla is None and not args.all_files:
        print(
            "Vanilla content not found — staging all scanned Redux files "
            "(install game or pass --vanilla)",
            file=sys.stderr,
        )

    rels = collect_changed(REDUX_CONTENT, vanilla, all_files=args.all_files or vanilla is None)
    stage(args.dest.resolve(), rels, REDUX_CONTENT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
