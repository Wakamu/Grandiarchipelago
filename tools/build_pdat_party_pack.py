#!/usr/bin/env python3
"""Assemble a stock-layout contiguous P_DAT party pack from charpack slices.

Stock battle load (+12C680, ebx+0x24 path → +12C860) does:

  form = ctx[0x243]
  row  = *(u16[4]*)(ctx + 0x7A200 + *(u32*)(ctx+0x7A200) + form*8)
  // row = {w0, w2, ?, w6}
  fread(P_DAT, off=w0<<11, size=(w2+w6)<<11, dest=ctx+0xC8A00)

So a clean custom party eventually wants a contiguous blob that looks like those
on-disk packs (headers + char bodies), not only external VirtualAlloc slices.

Per-char header (4x u32, pack-relative):
  [0] mesh / geometry
  [1] block B (often starts 00 01 40 00 …); [1]-[0] varies by char
  [2] block C; always [2]-[1] == 0x320 in playable packs seen
  [3] anim index table; always [3]-[2] == 0x14
  Anim table entries < 0x8000 are relative to [3] and may spill into the next char.

Usage:
  python tools/build_pdat_party_pack.py 1 3 4
  python tools/build_pdat_party_pack.py 1 2 5 6 --out data/packs/jfrm.bin
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from build_pdat_charpack import MAGIC, VERSION, ENTRY_SIZE, FILE_HDR  # noqa: E402
from pdat_char_catalog import NAMES  # noqa: E402

DEFAULT_CHARPACK = REPO / "data" / "pdat_charpack.bin"


def load_charpack_entries(path: Path) -> dict[int, dict]:
    data = path.read_bytes()
    magic, ver, count, _ = struct.unpack_from("<4sIII", data, 0)
    if magic != MAGIC or ver != VERSION:
        raise SystemExit(f"bad charpack {path}")
    preferred: dict[int, dict] = {}
    for i in range(count):
        off = FILE_HDR + i * ENTRY_SIZE
        cid, variant, flags, _pad = struct.unpack_from("<BBBB", data, off)
        hdr = struct.unpack_from("<4I", data, off + 4)
        blob_size, blob_off = struct.unpack_from("<II", data, off + 20)
        blob = data[blob_off : blob_off + blob_size]
        if (flags & 1) or variant == 0:
            preferred[cid] = {"char_id": cid, "hdr": list(hdr), "blob": blob}
    return preferred


def assemble_pack(chars: list[dict]) -> bytes:
    """Build stock-like pack: N headers (+ optional terminator) then bodies."""
    n = len(chars)
    if not (1 <= n <= 4):
        raise ValueError("party size 1..4")

    # Layout: header area, then each blob appended; rewrite hdr to pack-relative.
    header_bytes = (n + (0 if n == 4 else 1)) * 0x10
    out = bytearray(header_bytes)
    cursor = header_bytes

    for i, ch in enumerate(chars):
        blob = ch["blob"]
        # Charpack hdr is blob-relative; convert to pack-relative.
        pack_hdr = [cursor + h for h in ch["hdr"]]
        struct.pack_into("<4I", out, i * 0x10, *pack_hdr)
        out += blob
        # pad to 4
        while len(out) % 4:
            out += b"\0"
        cursor = len(out)

    if n < 4:
        # Terminator row: all-equal exclusive end
        end = len(out)
        struct.pack_into("<4I", out, n * 0x10, end, end, end, end)

    # Sector-align like on-disk packs (optional but matches fread size units)
    while len(out) % 0x800:
        out += b"\0"
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ids", nargs="+", type=int, help="Character ids (1..8), 1-4 of them")
    ap.add_argument("--charpack", type=Path, default=DEFAULT_CHARPACK)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    ids = args.ids
    if not (1 <= len(ids) <= 4):
        raise SystemExit("need 1..4 character ids")
    for i in ids:
        if i < 1 or i > 8:
            raise SystemExit(f"unsupported id {i} (playable battle cast is 1..8)")

    entries = load_charpack_entries(args.charpack)
    chars = []
    for i in ids:
        if i not in entries:
            raise SystemExit(f"char {i} missing from {args.charpack}")
        chars.append(entries[i])

    pack = assemble_pack(chars)
    sectors = len(pack) // 0x800
    label = "".join(NAMES.get(i, str(i))[0] for i in ids)
    out = args.out or (REPO / "data" / "packs" / f"party_{'_'.join(map(str, ids))}.bin")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(pack)

    print(f"Built {out}")
    print(f"  ids={ids} ({', '.join(NAMES.get(i,'?') for i in ids)})")
    print(f"  bytes={len(pack)} ({sectors} sectors) label~{label}")
    print("  headers:")
    for i in range(len(ids)):
        h = struct.unpack_from("<4I", pack, i * 0x10)
        print(f"    [{i}] {h}")
    if len(ids) < 4:
        print(f"    term {struct.unpack_from('<4I', pack, len(ids)*0x10)}")
    print()
    print("Stock install equivalent:")
    print(f"  memcpy(ctx+0xC8A00, pack, {len(pack)})")
    print("  // or point a form-row at a synthetic w0 with w2+w6 =", sectors)
    print("Note: packs >~184KiB overwrite stock scratch if placed contiguously at c8a;")
    print("      runtime may still need external blobs or a relocated dest.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
