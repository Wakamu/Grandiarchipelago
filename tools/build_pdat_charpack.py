#!/usr/bin/env python3
"""Build a pre-sliced battle character pack from P_DAT.BIN (GPD1 format).

Each playable gets one preferred blob with headers rebased to blob-relative
offsets, so the inject DLL can assemble arbitrary parties without parsing
multi-char P_DAT packs at fight time.

  python tools/build_pdat_charpack.py
  python tools/build_pdat_charpack.py --pdat ".../P_DAT.BIN" --out data/pdat_charpack.bin

Format (little-endian):
  magic      4  "GPD1"
  version    u32  (=1)
  count      u32
  reserved   u32  (=0)
  Entry[count]:
    char_id     u8
    variant     u8   (0 = preferred)
    flags       u8   (bit0 = preferred)
    pad         u8
    hdr[4]      u32  offsets relative to blob start
    blob_size   u32
    blob_off    u32  absolute file offset
  then 16-byte-aligned blobs
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from pdat_char_catalog import (  # noqa: E402
    DEFAULT_PDAT,
    NAMES,
    build_catalog,
    extract_span,
)

DEFAULT_OUT = REPO / "data" / "pdat_charpack.bin"
MAGIC = b"GPD1"
VERSION = 1
ENTRY_SIZE = 4 + 16 + 4 + 4  # meta + hdr[4] + size + off = 28
FILE_HDR = 16


def align16(n: int) -> int:
    return (n + 15) & ~15


def build_charpack(pdat: bytes, include_all_variants: bool = False) -> bytes:
    catalog = build_catalog(pdat)
    best = catalog["best_donor_by_char"]

    # Optional: also emit non-preferred fingerprints as variant>0
    variants: dict[int, list[dict]] = {int(k): [v] for k, v in best.items()}
    if include_all_variants:
        seen_fp: dict[int, set[str]] = {
            int(k): {v["fp"]} for k, v in best.items()
        }
        for pack in catalog["packs"]:
            for ch in pack["chars"]:
                cid = ch.get("char_id") or 0
                if not cid or "fp" not in ch:
                    continue
                fps = seen_fp.setdefault(cid, set())
                if ch["fp"] in fps:
                    continue
                # Skip spill-truncated / provisional last truncations
                if ch.get("spill_truncated"):
                    continue
                if "provisional" in (pack.get("confidence") or "") and ch.get("donor_last"):
                    continue
                fps.add(ch["fp"])
                variants.setdefault(cid, []).append(
                    {
                        "char_id": cid,
                        "name": NAMES.get(cid),
                        "label": pack["label"],
                        "w0": pack["w0"],
                        "w2": pack["w2"],
                        "w6": pack["w6"],
                        "index": ch["index"],
                        "span": ch["span"],
                        "fp": ch["fp"],
                        "hdr": ch["hdr"],
                        "start": ch["start"],
                        "end": ch["end"],
                        "donor_last": ch["donor_last"],
                    }
                )

    entries_meta: list[dict] = []
    blobs: list[bytes] = []

    for cid in sorted(variants.keys()):
        for vi, rec in enumerate(variants[cid]):
            w0 = rec["w0"]
            w2 = rec.get("w2")
            w6 = rec.get("w6")
            if w2 is None or w6 is None:
                # Fall back to sector gap from catalog pack entry
                pack_info = next(p for p in catalog["packs"] if p["w0"] == w0)
                size = pack_info["pack_bytes"]
            else:
                size = (w2 + w6) * 0x800
            pack = pdat[w0 * 0x800 : w0 * 0x800 + size]
            count = next(p["count"] for p in catalog["packs"] if p["w0"] == w0)
            span = extract_span(pack, rec["index"], count)
            if not span:
                raise RuntimeError(f"bad span for char {cid} from {rec['label']}")
            blob = pack[span["start"] : span["end"]]
            # Rebase headers to blob-relative
            hdr_rel = [h - span["start"] for h in span["hdr"]]
            if min(hdr_rel) < 0 or max(hdr_rel) >= len(blob):
                raise RuntimeError(f"rebased hdr OOB char={cid}: {hdr_rel} size={len(blob)}")
            preferred = 1 if vi == 0 else 0
            entries_meta.append(
                {
                    "char_id": cid,
                    "variant": vi,
                    "flags": preferred,
                    "hdr": hdr_rel,
                    "blob": blob,
                    "label": rec["label"],
                    "name": NAMES.get(cid, "?"),
                    "fp": rec["fp"],
                }
            )
            blobs.append(blob)

    n = len(entries_meta)
    index_bytes = n * ENTRY_SIZE
    blob_base = align16(FILE_HDR + index_bytes)

    out = bytearray()
    out += struct.pack("<4sIII", MAGIC, VERSION, n, 0)

    # Placeholder index; fill after we know blob offsets
    index_pos = len(out)
    out += b"\0" * index_bytes
    while len(out) < blob_base:
        out += b"\0"

    blob_offsets: list[int] = []
    for blob in blobs:
        blob_offsets.append(len(out))
        out += blob
        pad = align16(len(blob)) - len(blob)
        out += b"\0" * pad

    # Write index
    for i, meta in enumerate(entries_meta):
        off = index_pos + i * ENTRY_SIZE
        struct.pack_into(
            "<BBBB4III",
            out,
            off,
            meta["char_id"],
            meta["variant"],
            meta["flags"],
            0,
            meta["hdr"][0],
            meta["hdr"][1],
            meta["hdr"][2],
            meta["hdr"][3],
            len(meta["blob"]),
            blob_offsets[i],
        )

    # Manifest sidecar text for humans
    return bytes(out), entries_meta


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pdat", type=Path, default=DEFAULT_PDAT)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument(
        "--all-variants",
        action="store_true",
        help="Include non-preferred costume/era fingerprints as variant>0",
    )
    ap.add_argument(
        "--stage-native",
        action="store_true",
        help="Also copy to worlds/grandia/native/pdat_charpack.bin",
    )
    args = ap.parse_args()
    if not args.pdat.is_file():
        raise SystemExit(f"P_DAT not found: {args.pdat}")

    pdat = args.pdat.read_bytes()
    blob, metas = build_charpack(pdat, include_all_variants=args.all_variants)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    print(f"Wrote {args.out} ({len(blob)} bytes, {len(metas)} entries)")
    for m in metas:
        print(
            f"  id={m['char_id']} {m['name']:7} v={m['variant']} "
            f"pref={m['flags']} size={len(m['blob']):#x} from {m['label']} fp={m['fp']}"
        )

    manifest = args.out.with_suffix(".txt")
    lines = [
        f"GPD1 charpack from {args.pdat}",
        f"entries={len(metas)} bytes={len(blob)}",
        "",
    ]
    for m in metas:
        lines.append(
            f"id={m['char_id']} name={m['name']} variant={m['variant']} "
            f"preferred={m['flags']} size={len(m['blob'])} donor={m['label']} fp={m['fp']}"
        )
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")

    if args.stage_native:
        native = REPO / "worlds" / "grandia" / "native" / "pdat_charpack.bin"
        native.parent.mkdir(parents=True, exist_ok=True)
        native.write_bytes(blob)
        print(f"Staged {native}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
