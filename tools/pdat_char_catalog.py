#!/usr/bin/env python3
"""Catalog P_DAT.BIN party packs and per-character donor spans.

Grandia HD stores battle playable meshes/anims in content/BATLE/P_DAT.BIN as
pre-baked multi-character packs (not per-PGR). Each pack:
  - starts at sector w0 (byte_off = w0 * 0x800)
  - length (w2 + w6) * 0x800
  - begins with 0x10-byte headers per character (4x u32 pack-relative offsets)
  - optional terminator row (all four dwords equal) for count < 4

HD battle sprites live in BATLE/*.tpk — out of scope here.

Usage:
  python tools/pdat_char_catalog.py
  python tools/pdat_char_catalog.py --pdat "C:/.../P_DAT.BIN" --out data/pdat_char_catalog.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_PDAT = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\GRANDIA HD Remaster\content\BATLE\P_DAT.BIN"
)
DEFAULT_OUT = REPO / "data" / "pdat_char_catalog.json"

NAMES = {
    1: "Justin",
    2: "Feena",
    3: "Sue",
    4: "Gadwin",
    5: "Rapp",
    6: "Milda",
    7: "Guido",
    8: "Liete",
    11: "Leen",
    12: "Rem",
}

# Runtime-captured form rows (w0, w2, w6) + roster. See party_custom.cpp kPdatDonors.
KNOWN_PACKS: list[dict] = [
    {"label": "JS", "ids": [1, 3], "w0": 30, "w2": 38, "w6": 8},
    {"label": "JSF", "ids": [1, 3, 2], "w0": 76, "w2": 54, "w6": 11},
    {"label": "JF", "ids": [1, 2], "w0": 141, "w2": 42, "w6": 7},
    # Fingerprint-ID'd: Justin+Sue+Feena+Gadwin. Size from MGDAT {255,64,0,28}.
    {"label": "JSFG", "ids": [1, 3, 2, 4], "w0": 255, "w2": 64, "w6": 28, "confidence": "header+fp"},
    {"label": "JFG", "ids": [1, 2, 4], "w0": 408, "w2": 69, "w6": 9},
    {"label": "JFR", "ids": [1, 2, 5], "w0": 486, "w2": 53, "w6": 10},
    {"label": "JFRM", "ids": [1, 2, 5, 6], "w0": 549, "w2": 73, "w6": 12},
    {"label": "JFRG", "ids": [1, 2, 5, 7], "w0": 664, "w2": 66, "w6": 12},
    # Fingerprint-ID'd Justin+Rapp+Guido; w2/w6 = gap split (provisional).
    {"label": "JRG", "ids": [1, 5, 7], "w0": 772, "w2": 49, "w6": 12, "confidence": "fp; w2/w6 provisional"},
    {"label": "JFRL", "ids": [1, 2, 5, 8], "w0": 833, "w2": 71, "w6": 12},
    # Fingerprint-ID'd Justin+Rapp+Liete; w2/w6 provisional from sector gap.
    {"label": "JRL", "ids": [1, 5, 8], "w0": 916, "w2": 51, "w6": 12, "confidence": "fp; w2/w6 provisional"},
]


def sha12(blob: bytes) -> str:
    return hashlib.sha1(blob).hexdigest()[:12]


def read_hdr(pack: bytes, index: int) -> tuple[int, int, int, int]:
    return struct.unpack_from("<4I", pack, index * 0x10)


def find_pack_starts(data: bytes) -> list[int]:
    starts: list[int] = []
    for sec in range(len(data) // 0x800):
        off = sec * 0x800
        if off + 0x40 > len(data):
            break
        h0 = read_hdr(data[off : off + 0x40], 0)
        if h0[0] != 0x40:
            continue
        if not (0x1000 < h0[3] < 0x40000):
            continue
        if h0[1] < h0[0] or h0[2] < h0[0]:
            continue
        starts.append(sec)
    return starts


def anim_table_reach(pack: bytes, hdr: tuple[int, ...]) -> int:
    table = hdr[3]
    pack_size = len(pack)
    if table == 0 or table + 0x40 > pack_size:
        return 0
    reach = table + 0x100
    for i in range(64):
        ent_off = table + i * 4
        if ent_off + 4 > pack_size:
            break
        entry = struct.unpack_from("<I", pack, ent_off)[0]
        if 0 < entry < 0x8000:
            end = table + entry + 0x200
            if end > reach:
                reach = end
    return min(reach, pack_size)


def extract_span(
    pack: bytes, index: int, count: int, spill_cap: int = 0x9000
) -> dict | None:
    """Mirror party_custom.cpp ExtractPdatCharSpan (with updated spill cap)."""
    pack_size = len(pack)
    hdr = read_hdr(pack, index)
    data_start = min(hdr)
    data_end = max(hdr)
    boundary = pack_size
    if index + 1 < count:
        next_hdr = read_hdr(pack, index + 1)
        if data_start < next_hdr[0] <= pack_size:
            boundary = next_hdr[0]
    else:
        if (count * 0x10 + 0x10) <= pack_size:
            term = read_hdr(pack, count)
            if term[0] == term[1] == term[2] == term[3] and data_end <= term[0] <= pack_size:
                boundary = term[0]
    data_end = max(data_end, boundary)
    anim_reach = anim_table_reach(pack, hdr)
    spill_needed = max(0, anim_reach - boundary)
    if anim_reach > data_end:
        data_end = min(anim_reach, boundary + spill_cap, pack_size)
    if data_end <= data_start:
        return None
    return {
        "hdr": list(hdr),
        "start": data_start,
        "end": data_end,
        "span": data_end - data_start,
        "boundary": boundary,
        "anim_reach": anim_reach,
        "spill_needed": spill_needed,
        "spill_truncated": spill_needed > spill_cap,
        "donor_last": index + 1 >= count,
    }


def anim_oob_past_boundary(pack: bytes, index: int, count: int) -> list[dict]:
    hdr = read_hdr(pack, index)
    table = hdr[3]
    if index + 1 < count:
        boundary = read_hdr(pack, index + 1)[0]
    else:
        term = read_hdr(pack, count) if (count * 0x10 + 0x10) <= len(pack) else None
        if term and term[0] == term[1] == term[2] == term[3]:
            boundary = term[0]
        else:
            boundary = len(pack)
    oob: list[dict] = []
    for j in range(64):
        entry = struct.unpack_from("<I", pack, table + j * 4)[0]
        if 0 < entry < 0x8000:
            abs_off = table + entry
            if abs_off >= boundary:
                oob.append({"slot": j, "rel": entry, "abs": abs_off})
    return oob


def build_catalog(data: bytes) -> dict:
    starts = find_pack_starts(data)
    known_by_w0 = {p["w0"]: p for p in KNOWN_PACKS}
    packs_out: list[dict] = []

    for i, sec in enumerate(starts):
        known = known_by_w0.get(sec)
        if known:
            size = (known["w2"] + known["w6"]) * 0x800
            count = len(known["ids"])
            ids = list(known["ids"])
            label = known["label"]
            confidence = known.get("confidence", "runtime capture")
        else:
            end_sec = starts[i + 1] if i + 1 < len(starts) else len(data) // 0x800
            size = (end_sec - sec) * 0x800
            # Infer count from terminator / 4-char
            pack_probe = data[sec * 0x800 : sec * 0x800 + min(size, 0x8000)]
            count = 1
            for n in range(1, 5):
                if n * 0x10 + 0x10 > len(pack_probe):
                    break
                h = read_hdr(pack_probe, n)
                if h[0] == h[1] == h[2] == h[3] and h[0] >= 0x40:
                    count = n
                    break
                count = n
            ids = []
            label = f"sec{sec}"
            confidence = "header scan only"

        pack = data[sec * 0x800 : sec * 0x800 + size]
        if len(pack) < size:
            continue

        chars: list[dict] = []
        for ci in range(count):
            span = extract_span(pack, ci, count)
            if not span:
                chars.append({"index": ci, "error": "bad span"})
                continue
            blob = pack[span["start"] : span["end"]]
            char_id = ids[ci] if ci < len(ids) else 0
            oob = anim_oob_past_boundary(pack, ci, count)
            chars.append(
                {
                    "index": ci,
                    "char_id": char_id,
                    "name": NAMES.get(char_id, "?"),
                    "fp": sha12(blob),
                    "anim_oob_past_boundary": len(oob),
                    "anim_oob_sample": oob[:6],
                    **span,
                }
            )

        packs_out.append(
            {
                "label": label,
                "w0": sec,
                "w2": known["w2"] if known else None,
                "w6": known["w6"] if known else None,
                "pack_bytes": size,
                "count": count,
                "ids": ids,
                "confidence": confidence,
                "chars": chars,
            }
        )

    # Fingerprint → char_id from known packs
    fp_to_ids: dict[str, set[int]] = {}
    for p in packs_out:
        for ch in p["chars"]:
            if ch.get("char_id"):
                fp_to_ids.setdefault(ch["fp"], set()).add(ch["char_id"])

    for p in packs_out:
        if p["ids"]:
            continue
        guessed: list[int] = []
        for ch in p["chars"]:
            ids = fp_to_ids.get(ch.get("fp", ""), set())
            if len(ids) == 1:
                cid = next(iter(ids))
                ch["char_id"] = cid
                ch["name"] = NAMES.get(cid, "?")
                guessed.append(cid)
            else:
                ch["guess_ids"] = sorted(ids)
                guessed.append(0)
        p["ids_guessed"] = guessed

    # Best donor scoring (lower wins):
    # - never prefer spill-truncated slices
    # - prefer mid-pack (clean next-header end) over last-in-pack
    # - prefer runtime-captured form rows over fingerprint-only packs
    # - for last-in-pack chars, prefer *larger* span (provisional sizes truncate)
    # - for mid-pack chars, prefer smaller complete span
    best: dict[int, dict] = {}
    for p in packs_out:
        provisional = "provisional" in (p.get("confidence") or "")
        for ch in p["chars"]:
            cid = ch.get("char_id") or 0
            if not cid or "span" not in ch:
                continue
            score = 0
            if ch.get("spill_truncated"):
                score += 0x04000000
            if ch["donor_last"]:
                score += 0x01000000
                score += max(0, 0x200000 - ch["span"])  # larger last-span better
            else:
                score += ch["span"]
            if provisional:
                score += 0x00800000
            score += (p["pack_bytes"] or 0) // 64
            prev = best.get(cid)
            if prev is None or score < prev["score"]:
                best[cid] = {
                    "char_id": cid,
                    "name": NAMES.get(cid),
                    "label": p["label"],
                    "w0": p["w0"],
                    "w2": p["w2"],
                    "w6": p["w6"],
                    "index": ch["index"],
                    "span": ch["span"],
                    "spill_needed": ch["spill_needed"],
                    "spill_truncated": ch["spill_truncated"],
                    "donor_last": ch["donor_last"],
                    "fp": ch["fp"],
                    "hdr": ch["hdr"],
                    "start": ch["start"],
                    "end": ch["end"],
                    "score": score,
                    "confidence": p.get("confidence"),
                }

    max_spill = 0
    for p in packs_out:
        for ch in p["chars"]:
            max_spill = max(max_spill, ch.get("spill_needed") or 0)

    return {
        "pdat_size": len(data),
        "pack_starts_sec": starts,
        "max_anim_spill_needed": max_spill,
        "recommended_anim_spill_cap": max(0x9000, (max_spill + 0xFF) & ~0xFF),
        "packs": packs_out,
        "best_donor_by_char": {str(k): v for k, v in sorted(best.items())},
        "notes": [
            "Battle HD sprites are BATLE/*.tpk, not PGR CPD or these packs.",
            "Char blobs are pack-relative; c8a headers store offsets from c8a base.",
            "Anim table entries < 0x8000 are relative to the table and often spill "
            "past the next character's mesh start (stock overlap). Slice end must "
            "include that spill or mid-pack donors are incomplete.",
            "Justin/Feena/Sue/Rapp have multiple fingerprints across story packs "
            "(costume/era variants) — pick the donor matching the field era when possible.",
            "Gadwin/Milda/Guido/Liete only appear as last slot in known packs.",
        ],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pdat", type=Path, default=DEFAULT_PDAT)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    if not args.pdat.is_file():
        raise SystemExit(f"P_DAT not found: {args.pdat}")

    data = args.pdat.read_bytes()
    catalog = build_catalog(data)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(catalog, indent=2), encoding="utf-8")

    print(f"P_DAT {catalog['pdat_size']} bytes, {len(catalog['packs'])} packs")
    print(
        f"max anim spill={catalog['max_anim_spill_needed']:#x} "
        f"recommended cap={catalog['recommended_anim_spill_cap']:#x}"
    )
    print("\nBest donors:")
    for rec in catalog["best_donor_by_char"].values():
        print(
            f"  {rec['name']:7} <- {rec['label']}[{rec['index']}] "
            f"w0={rec['w0']} span={rec['span']:#x} spill={rec['spill_needed']:#x} "
            f"last={rec['donor_last']} trunc={rec['spill_truncated']}"
        )
    print(f"\nWrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
