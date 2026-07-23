"""Grandia HD Remaster Archipelago client (CommonClient).

Connects to Archipelago like the official Text Client / FF7 client, injects
``Grandiarchipelago.dll`` into ``grandia.exe``, then bridges checks/items over
the named pipe.
"""

from __future__ import annotations

import asyncio
import os
import sys
import time
from pathlib import Path
from typing import List, Optional, Set

from CommonClient import ClientCommandProcessor, CommonContext, get_base_parser, gui_enabled, logger, server_loop
from Utils import async_start

from .dll_inject import find_process_id, inject_dll
from .game_pipe import GamePipe

try:
    from NetUtils import ItemClassification
except ImportError:  # pragma: no cover
    from BaseClasses import ItemClassification

ITEM_DELIVERY_DELAY_S = 2.5
PROCESS_NAME = "grandia.exe"
POLL_S = 0.25
# Fake AP progression ids: Key to <Map> = 0x47523000 + map_id (not stash rows).
MAP_KEY_ITEM_BASE = 0x47523000
AREA_LOCKOUT_ITEM_BASE = 0x47540000
# Must match worlds/grandia/Locations.py
CHEST_EVENT_LOCATION_BASE = 0x47522000
AREA_LOCKOUT_LOCATION_BASE = 0x47524000
# NetworkItem.flags bit for progression (includes progression_skip_balancing).
_PROGRESSION_FLAG = int(ItemClassification.progression)


def _fnv1a32(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h if h != 0 else 1


def compute_seed_hash(seed_name: str, slot: int) -> int:
    """Match Bridge ComputeSeedHash: FNV-1a of UTF-8 'seed\\0slot' (never 0)."""
    return _fnv1a32(f"{seed_name}\0{slot}".encode("utf-8"))


def _is_map_key_item(item_id: int) -> bool:
    return MAP_KEY_ITEM_BASE <= item_id < AREA_LOCKOUT_ITEM_BASE


def _is_lockout_item(item_id: int) -> bool:
    return AREA_LOCKOUT_ITEM_BASE <= item_id < AREA_LOCKOUT_ITEM_BASE + 0x10000


def _package_native_bytes(rel_posix: str) -> Optional[bytes]:
    """Read a file under native/ from the installed world (folder or .apworld)."""
    fs_path = Path(__file__).resolve().parent / Path(*rel_posix.split("/"))
    if fs_path.is_file():
        return fs_path.read_bytes()

    try:
        from importlib.resources import files

        resource = files(__package__).joinpath(rel_posix)
        return resource.read_bytes()
    except Exception:
        return None


def _package_native_dll_bytes() -> Optional[bytes]:
    """Read bundled native/Grandiarchipelago.dll from the installed world (folder or .apworld)."""
    return _package_native_bytes("native/Grandiarchipelago.dll")


def _cache_native_dir() -> Path:
    try:
        from Utils import user_path

        return Path(user_path("Grandia", "native"))
    except Exception:
        return Path.home() / "Archipelago" / "Grandia" / "native"


def _extract_native_file(rel_posix: str, dest: Path, label: str) -> bool:
    """Write packaged native/* bytes to dest when missing or hash-stale."""
    import hashlib

    data = _package_native_bytes(rel_posix)
    if data is None:
        return False
    dest.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(data).hexdigest()
    marker = Path(str(dest) + ".sha256")
    if (
        not dest.is_file()
        or not marker.is_file()
        or marker.read_text(encoding="utf-8").strip() != digest
    ):
        dest.write_bytes(data)
        marker.write_text(digest + "\n", encoding="utf-8")
        logger.info("Extracted bundled %s to %s", label, dest)
    return True


def _ensure_bundled_dll() -> Optional[Path]:
    """
    Prefer the DLL shipped inside the APWorld. LoadLibrary needs a real file path,
    so when the world is a .apworld zip we extract to the Archipelago user cache.
    Also extracts Redux FIELD overlays (SHOP.BIN + map MDPs) beside the DLL.
    """
    data = _package_native_dll_bytes()
    if data is None:
        return None

    # Source / folder install: inject straight from native/ when it is a real path.
    fs_path = Path(__file__).resolve().parent / "native" / "Grandiarchipelago.dll"
    if fs_path.is_file() and fs_path.stat().st_size == len(data):
        # Only use in-place if not stuck inside a zip path (zipimport __file__ is weird).
        try:
            if fs_path.exists() and not str(fs_path).lower().endswith(".apworld"):
                # zipimport often sets __file__ to ".../grandia.apworld/grandia/..."
                parts_lower = [p.lower() for p in fs_path.parts]
                if not any(p.endswith(".apworld") for p in parts_lower):
                    return fs_path
        except OSError:
            pass

    cache_dir = _cache_native_dir()
    cache_dir.mkdir(parents=True, exist_ok=True)
    dest = cache_dir / "Grandiarchipelago.dll"
    if not _extract_native_file("native/Grandiarchipelago.dll", dest, "inject DLL"):
        return None
    _extract_redux_content(cache_dir)
    return dest.resolve()


def _extract_redux_content(cache_dir: Path) -> None:
    """Extract packaged redux_content overlays (FIELD/BIN/BATLE/TEXT) next to the DLL."""
    import json

    root = cache_dir / "redux_content"
    names: list[str] = []
    man_bytes = _package_native_bytes("native/redux_content/manifest.json")
    if man_bytes:
        try:
            names = list(json.loads(man_bytes.decode("utf-8")).get("files") or [])
        except Exception:
            names = []
        _extract_native_file(
            "native/redux_content/manifest.json",
            root / "manifest.json",
            "Redux overlay manifest",
        )
    if not names:
        # Fallback: minimum set for Parm shop + MCHAR.
        names = [
            "FIELD/SHOP.BIN",
            "FIELD/204C.MDP",
            "FIELD/WINDT.BIN",
            "BIN/MCHAR.DAT",
            "BATLE/M_DAT.BIN",
            "TEXT/EN/TEXT1.BIN",
            "TEXT/EN/strings.txt",
        ]
    for rel in names:
        rel = rel.replace("\\", "/").lstrip("/")
        # Legacy manifests listed bare FIELD basenames only.
        if "/" not in rel:
            rel = f"FIELD/{rel}"
        _extract_native_file(
            f"native/redux_content/{rel}",
            root / Path(*rel.split("/")),
            f"Redux {rel}",
        )


def _resolve_dll_path() -> Optional[Path]:
    env = os.environ.get("GRANDIA_AP_DLL")
    if env:
        p = Path(env)
        if p.is_file():
            return p.resolve()

    # Prefer a local Win32 build when developing from the repo tree.
    build_dll = (
        Path(__file__).resolve().parents[2]
        / "client"
        / "build"
        / "Release"
        / "Grandiarchipelago.dll"
    )
    if build_dll.is_file():
        return build_dll.resolve()

    bundled = _ensure_bundled_dll()
    if bundled is not None:
        return bundled

    candidates = [
        Path.cwd() / "Grandiarchipelago.dll",
    ]
    try:
        from Utils import user_path

        candidates.insert(0, Path(user_path()) / "Grandiarchipelago.dll")
        candidates.insert(1, Path(user_path("Grandia")) / "Grandiarchipelago.dll")
    except Exception:
        pass

    for path in candidates:
        if path.is_file():
            return path.resolve()
    return None


class GrandiaCommandProcessor(ClientCommandProcessor):
    def _cmd_attach(self) -> bool:
        """Force re-attach / re-inject into grandia.exe."""
        assert isinstance(self.ctx, GrandiaContext)
        self.ctx.request_reattach = True
        self.ctx._awaiting_launch = True
        self.ctx._logged_skip_running = False
        self.output("Will re-attach to grandia.exe on the next watcher tick.")
        return True

    def _cmd_sync(self) -> bool:
        """Print current save SYNC / applied item index."""
        assert isinstance(self.ctx, GrandiaContext)
        self.output(
            f"SYNC={self.ctx.last_sync_index} applied={self.ctx.applied_index} "
            f"items_received={len(self.ctx.items_received)} game_pipe="
            f"{'up' if self.ctx.pipe and self.ctx.pipe.connected else 'down'}"
        )
        return True

    def _cmd_toast(self, *parts: str) -> bool:
        """Send overlay text to the game. Usage: /toast [#RRGGBB] Hello world"""
        assert isinstance(self.ctx, GrandiaContext)
        tokens = list(parts)
        color = None
        if tokens and tokens[0].startswith("#") and len(tokens[0]) == 7:
            color = tokens.pop(0)
        text = " ".join(tokens).strip()
        if not text:
            self.output("Usage: /toast [#RRGGBB] <message>")
            return False
        if not self.ctx.pipe or not self.ctx.pipe.connected:
            self.output("Game pipe is not connected.")
            return False
        self.ctx._send_toast(text, color)
        self.output(f"Toast sent: {text}" + (f" ({color})" if color else ""))
        return True

    def _cmd_toast_clear(self) -> bool:
        """Clear the in-game overlay toast."""
        assert isinstance(self.ctx, GrandiaContext)
        if not self.ctx.pipe or not self.ctx.pipe.connected:
            self.output("Game pipe is not connected.")
            return False
        self.ctx.pipe.send_line("TOAST_CLEAR")
        self.output("Toast cleared.")
        return True


class GrandiaContext(CommonContext):
    game = "Grandia"
    items_handling = 0b111  # AllItems
    command_processor = GrandiaCommandProcessor

    def __init__(self, server_address: Optional[str], password: Optional[str]) -> None:
        super().__init__(server_address, password)
        self.pipe: Optional[GamePipe] = None
        self.dll_path: Optional[Path] = _resolve_dll_path()
        self.hook_injected = False
        self.request_reattach = False
        # Inject only after we've seen grandia.exe absent (or /attach). Pre-existing
        # processes are ignored until relaunch.
        self._awaiting_launch = False
        self._injected_pid: Optional[int] = None
        self._logged_skip_running = False
        self.last_sync_index: Optional[int] = None
        self.applied_index = 0
        self.delivery_open = False
        self.save_bound = False
        self.expected_seed_hash = 0
        self._delivery_open_at = 0.0
        self._forwarded_indexes: Set[int] = set()
        self._pending_sync: Optional[int] = None
        self._pending_lockouts: List[tuple[int, List[int]]] = []
        self._scouted_all = False
        self.slot_data: dict = {}

    def _refresh_seed_hash(self, seed_name: Optional[str] = None, slot: Optional[int] = None) -> None:
        """Recompute save-binding hash from room seed + slot number."""
        if seed_name is None:
            seed_name = getattr(self, "seed_name", None) or ""
        if slot is None:
            slot = int(getattr(self, "slot", 0) or 0)
        slot = int(slot or 0)
        if slot <= 0:
            self.expected_seed_hash = 0
            return
        self.expected_seed_hash = compute_seed_hash(str(seed_name or ""), slot)
        logger.info(
            "Save binding seed_hash=0x%08X (seed=%r, slot=%s)",
            self.expected_seed_hash,
            seed_name or "",
            slot,
        )

    def make_gui(self):
        ui = super().make_gui()
        ui.base_title = "Archipelago Grandia Client"
        return ui

    async def server_auth(self, password_requested: bool = False) -> None:
        if password_requested and not self.password:
            await super().server_auth(password_requested)
        await self.get_username()
        await self.send_connect()

    def on_package(self, cmd: str, args: dict) -> None:
        # Capture RoomInfo seed before Connected can race (CommonClient calls
        # server_auth during RoomInfo *before* on_package(RoomInfo)).
        if cmd == "RoomInfo":
            seed_name = args.get("seed_name")
            if seed_name:
                self.seed_name = seed_name

        if cmd == "Connected":
            # CommonClient does not assign this; worlds must read it from Connected.
            self.slot_data = args.get("slot_data") or {}
            logger.info("Connected to Archipelago as %s", self.auth)
            if isinstance(self.slot_data, dict) and self.slot_data:
                logger.info(
                    "slot_data: %s",
                    " ".join(f"{k}={self.slot_data[k]!r}" for k in sorted(self.slot_data)),
                )
            else:
                logger.warning(
                    "Connected with empty slot_data — regenerate the seed with the current Grandia world, "
                    "or the room was created before these options existed."
                )
            logger.info(
                "Holding item delivery until a save bound to this seed/slot is loaded (or first-bound)."
            )
            # CommonClient sets self.slot before on_package; prefer packet field anyway.
            seed_name = getattr(self, "seed_name", None) or ""
            slot = int(args.get("slot") or getattr(self, "slot", 0) or 0)
            self._refresh_seed_hash(seed_name, slot)
            self.save_bound = False
            self._scouted_all = False
            async_start(self._scout_missing_locations(), name="grandia-scout")
            self._send_toast(f"Connected as {self.auth}", "#FFFF00")
            self._push_runtime_config()
        elif cmd == "LocationInfo":
            # Scouts finished (or partial) — drain any lockouts waiting on item data.
            async_start(self._drain_pending_lockouts(), name="grandia-lockout-drain")
        elif cmd == "RoomInfo":
            seed_name = getattr(self, "seed_name", None) or args.get("seed_name") or ""
            # RoomInfo often arrives before Connected; hash is finalized on Connected.
            # If Connected already ran (auth race), refresh so HELLO/CONFIG can stamp.
            if getattr(self, "slot", None):
                self._refresh_seed_hash(seed_name, int(self.slot))
                self._push_runtime_config()
        super().on_package(cmd, args)
        # After CommonClient assigns self.slot, ensure hash is non-zero.
        if cmd == "Connected" and not self.expected_seed_hash:
            self._refresh_seed_hash()
            self._push_runtime_config()

    def _push_runtime_config(self) -> None:
        if not self.pipe or not self.pipe.connected:
            return
        slot_data = getattr(self, "slot_data", None) or {}

        def _flag(key: str, default: int = 1) -> int:
            if not isinstance(slot_data, dict) or key not in slot_data:
                return default
            try:
                return 1 if int(slot_data[key]) else 0
            except (TypeError, ValueError):
                return 1 if slot_data[key] else 0

        def _int(key: str, default: int = 1, lo: int = 1, hi: int = 100) -> int:
            if not isinstance(slot_data, dict) or key not in slot_data:
                return default
            try:
                value = int(slot_data[key])
            except (TypeError, ValueError):
                return default
            return max(lo, min(hi, value))

        flags = {
            "include_gold_chests": _flag("include_gold_chests"),
            "include_soldiers_graveyard": _flag("include_soldiers_graveyard"),
            "include_castle_of_dreams": _flag("include_castle_of_dreams"),
            "include_tower_of_temptation": _flag("include_tower_of_temptation"),
            "magic_xp_multiplier": _int("magic_xp_multiplier"),
            "skill_xp_multiplier": _int("skill_xp_multiplier"),
            "level_xp_multiplier": _int("level_xp_multiplier"),
            "gameplay_balance": _int(
                "gameplay_balance",
                default=_int("enemy_data_pack", default=0, lo=0, hi=1),
                lo=0,
                hi=1,
            ),
        }
        if self.expected_seed_hash:
            flags["seed_hash"] = self.expected_seed_hash
        for key, value in flags.items():
            self.pipe.send_line(f"CONFIG {key} {value}")
        logger.info("Pushed CONFIG %s", " ".join(f"{k}={v}" for k, v in flags.items()))

    def on_print_json(self, args: dict) -> None:
        super().on_print_json(args)
        self._toast_from_print_json(args)

    def _send_toast(self, message: str, color_hex: Optional[str] = None) -> None:
        if not self.pipe or not self.pipe.connected:
            return
        cleaned = " ".join(str(message).replace("\r", " ").replace("\n", " ").split())
        if not cleaned:
            return
        if len(cleaned) > 120:
            cleaned = cleaned[:117] + "..."
        if color_hex:
            hex_part = color_hex.strip()
            if not hex_part.startswith("#"):
                hex_part = f"#{hex_part}"
            self.pipe.send_line(f"TOAST {hex_part} {cleaned}")
        else:
            self.pipe.send_line(f"TOAST {cleaned}")

    @staticmethod
    def _color_hex_for_item_flags(flags: int) -> str:
        # Match Archipelago.MultiClient.Net Models.Color (dark client palette).
        if flags & int(ItemClassification.trap):
            return "#FA8072"  # Salmon
        if flags & int(ItemClassification.progression):
            return "#DDA0DD"  # Plum
        if flags & int(ItemClassification.useful):
            return "#6A5ACD"  # SlateBlue
        return "#FFFFFF"  # White (filler / default)

    def _toast_from_print_json(self, args: dict) -> None:
        if args.get("type") != "ItemSend":
            return
        if self.is_uninteresting_item_send(args):
            return

        item = args.get("item")
        receiving = args.get("receiving")
        if item is None or receiving is None:
            return

        try:
            sender_slot = int(item.player)
            item_id = int(item.item)
            recv_slot = int(receiving)
            flags = int(getattr(item, "flags", 0) or 0)
        except (AttributeError, TypeError, ValueError):
            return

        # NetworkItem.item is an id in the *receiver's* game, not the finder's.
        item_name = self._item_name_for_send(item_id, recv_slot)
        color = self._color_hex_for_item_flags(flags)

        sender_name = self.player_names.get(sender_slot, f"Player {sender_slot}")
        recv_name = self.player_names.get(recv_slot, f"Player {recv_slot}")

        if self.slot_concerns_self(recv_slot) and self.slot_concerns_self(sender_slot):
            self._send_toast(f"Found {item_name}", color)
        elif self.slot_concerns_self(recv_slot):
            self._send_toast(f"Received {item_name} from {sender_name}", color)
        elif self.slot_concerns_self(sender_slot):
            self._send_toast(f"Sent {item_name} to {recv_name}", color)
    def _item_name_for_send(self, item_id: int, recv_slot: int) -> str:
        try:
            name = self.item_names.lookup_in_slot(item_id, recv_slot)
            if name and str(name).lower() not in ("unknown item", "unknown"):
                return str(name)
        except Exception:
            pass
        try:
            info = self.slot_info.get(recv_slot)
            if info is not None:
                name = self.item_names.lookup_in_game(item_id, info.game)
                if name and str(name).lower() not in ("unknown item", "unknown"):
                    return str(name)
        except Exception:
            pass
        return f"Item 0x{item_id:X}"

    async def _scout_missing_locations(self) -> None:
        locs = [int(x) for x in self.missing_locations]
        if not locs:
            self._scouted_all = True
            return
        await self.send_msgs(
            [{"cmd": "LocationScouts", "locations": locs, "create_as_hint": 0}]
        )
        self._scouted_all = True
        logger.debug("Scouted %u missing locations for lockout progression filtering", len(locs))

    @staticmethod
    def _is_progression_flags(flags: int) -> bool:
        return bool(int(flags) & _PROGRESSION_FLAG)

    def _progression_locations(self, location_ids: List[int]) -> List[int]:
        out: List[int] = []
        for loc_id in location_ids:
            if loc_id in self.locations_checked:
                continue
            info = self.locations_info.get(loc_id)
            if info is None:
                continue
            if self._is_progression_flags(getattr(info, "flags", 0)):
                out.append(loc_id)
        return out

    async def _handle_lockout(self, event_id: int, location_ids: List[int]) -> None:
        if not location_ids:
            return

        missing_info = [lid for lid in location_ids if lid not in self.locations_info]
        if missing_info:
            self._pending_lockouts.append((event_id, location_ids))
            await self.send_msgs(
                [{"cmd": "LocationScouts", "locations": missing_info, "create_as_hint": 0}]
            )
            return

        await self._apply_lockout_checks(event_id, location_ids)

    async def _drain_pending_lockouts(self) -> None:
        if not self._pending_lockouts:
            return
        pending = self._pending_lockouts
        self._pending_lockouts = []
        still_waiting: List[tuple[int, List[int]]] = []
        for event_id, location_ids in pending:
            if any(lid not in self.locations_info for lid in location_ids):
                still_waiting.append((event_id, location_ids))
                continue
            await self._apply_lockout_checks(event_id, location_ids)
        self._pending_lockouts.extend(still_waiting)

    async def _apply_lockout_checks(self, event_id: int, location_ids: List[int]) -> None:
        prog = self._progression_locations(location_ids)
        to_check: Set[int] = set(prog)
        # Companion lockout location (locked event item) for UT region seals.
        companion = AREA_LOCKOUT_LOCATION_BASE + int(event_id)
        if companion in self.missing_locations:
            to_check.add(companion)
        if to_check:
            await self.check_locations(to_check)

    def apply_sync(self, received_index: int, save_seed_hash: int = 0, has_trailer: bool = False) -> None:
        if not self.expected_seed_hash:
            logger.warning("Save SYNC ignored — not connected to an AP room yet.")
            self._send_toast("Connect to Archipelago before loading a save", "#FA8072")
            return

        if not has_trailer and save_seed_hash == 0:
            self.save_bound = False
            self.delivery_open = False
            self._pending_sync = None
            logger.warning(
                "Save SYNC rejected — vanilla save (no GAP1, received_index=%s). "
                "Start a New Game while connected, then Save to bind this slot.",
                received_index,
            )
            self._send_toast("Vanilla save — New Game + Save required for AP", "#FA8072")
            return

        if has_trailer and save_seed_hash == 0:
            self.save_bound = False
            self.delivery_open = False
            self._pending_sync = None
            logger.warning(
                "Save SYNC rejected — legacy GAP1 (seed=0, received_index=%s). "
                "Start a New Game while connected, then Save to bind this slot.",
                received_index,
            )
            self._send_toast("Legacy AP save — New Game + Save required for AP", "#FA8072")
            return

        if save_seed_hash != self.expected_seed_hash:
            self.save_bound = False
            self.delivery_open = False
            self._pending_sync = None
            logger.warning(
                "Save SYNC rejected — seed mismatch save=0x%08X expected=0x%08X",
                save_seed_hash,
                self.expected_seed_hash,
            )
            self._send_toast("Save belongs to a different AP seed/slot", "#FA8072")
            return

        self.save_bound = True
        self._forwarded_indexes = {i for i in self._forwarded_indexes if i <= received_index}
        self.applied_index = received_index
        self.last_sync_index = received_index
        self.delivery_open = False
        self._pending_sync = received_index
        self._delivery_open_at = time.monotonic() + ITEM_DELIVERY_DELAY_S
        logger.info(
            "Save SYNC OK seed=0x%08X received_index=%s — re-applying map keys + Index > %s after %.1fs",
            save_seed_hash,
            received_index,
            received_index,
            ITEM_DELIVERY_DELAY_S,
        )

    def reapply_map_keys(self) -> None:
        """Map keys are runtime-only in the DLL. After save load, SYNC clears them and
        catch-up only forwards Index > watermark — re-send every key already counted in
        the save as ITEM without INDEX (must not rewrite GAP1 received_index).
        Newer keys (Index > watermark) are delivered by catch-up as usual.
        """
        if not self.pipe or not self.pipe.connected:
            return
        # Index N ↔ items_received[N - 1]; only re-apply Index ≤ applied_index.
        limit = min(self.applied_index, len(self.items_received))
        sent = 0
        for i in range(limit):
            item = self.items_received[i]
            item_id = int(item.item)
            if not _is_map_key_item(item_id):
                continue
            self.pipe.send_line(f"ITEM 0x{item_id:X}")
            logger.info("Re-applied map key after load: 0x%X", item_id)
            sent += 1
        if sent:
            logger.info(
                "Re-applied %s map key(s) from items_received (Index ≤ %s).",
                sent,
                self.applied_index,
            )

    def catch_up_items(self) -> None:
        if not self.pipe or not self.pipe.connected or not self.delivery_open:
            return
        # Index N corresponds to items_received[N - 1]
        for i in range(self.applied_index, len(self.items_received)):
            index = i + 1
            item = self.items_received[i]
            self._forward_item(index, int(item.item), item)

    def _forward_item(self, index: int, item_id: int, item) -> None:
        if index <= self.applied_index:
            return
        if index in self._forwarded_indexes:
            return
        if not self.pipe or not self.pipe.connected:
            return
        # Logic-only tokens — still advance the watermark so catch-up does not stall.
        if _is_lockout_item(item_id):
            self._forwarded_indexes.add(index)
            self.applied_index = index
            return
        self.pipe.send_line(f"ITEM 0x{item_id:X} INDEX {index}")
        self._forwarded_indexes.add(index)
        self.applied_index = index

    def handle_pipe_line(self, line: str) -> None:
        if line.startswith("HELLO"):
            logger.info("Game said: %s", line)
            self.pipe and self.pipe.send_line("CONNECTED")
            self._push_runtime_config()
            return
        if line.startswith("SYNC "):
            parts = line[5:].split()
            if not parts:
                logger.warning("Invalid SYNC line: %s", line)
                return
            try:
                index = int(parts[0])
            except ValueError:
                logger.warning("Invalid SYNC line: %s", line)
                return
            if index < 0:
                return
            save_seed = 0
            has_trailer = False
            if len(parts) >= 2:
                try:
                    save_seed = int(parts[1], 0)
                except ValueError:
                    save_seed = 0
            if len(parts) >= 3:
                try:
                    has_trailer = int(parts[2], 0) != 0
                except ValueError:
                    has_trailer = False
            self.apply_sync(index, save_seed, has_trailer)
            return
        if line.startswith("CHECK "):
            if not self.save_bound:
                logger.warning("Ignored check — save not bound to this AP seed/slot: %s", line)
                return
            token = line[6:].strip().replace("0x", "").replace("0X", "")
            try:
                location_id = int(token, 16)
            except ValueError:
                try:
                    location_id = int(token)
                except ValueError:
                    logger.warning("Invalid CHECK line: %s", line)
                    return
            async_start(self.check_locations({location_id}), name="grandia-check")
            # If this is a story event check, also clear its Area Lockout companion when present.
            if location_id >= CHEST_EVENT_LOCATION_BASE and location_id < AREA_LOCKOUT_LOCATION_BASE:
                event_id = location_id - CHEST_EVENT_LOCATION_BASE
                companion = AREA_LOCKOUT_LOCATION_BASE + event_id
                if companion in self.missing_locations:
                    async_start(self.check_locations({companion}), name="grandia-lockout-companion")
            return
        if line.startswith("LOCKOUT "):
            if not self.save_bound:
                return
            parts = line.split()
            if len(parts) < 2:
                logger.warning("Invalid LOCKOUT line: %s", line)
                return
            try:
                event_id = int(parts[1], 16)
                location_ids = [int(tok, 16) for tok in parts[2:]]
            except ValueError:
                logger.warning("Invalid LOCKOUT line: %s", line)
                return
            async_start(self._handle_lockout(event_id, location_ids), name="grandia-lockout")
            return
        logger.debug("Game pipe: %s", line)


async def game_watcher(ctx: GrandiaContext) -> None:
    logger.info("Game watcher started — connect to Archipelago, then launch Grandia past the Steam menu.")
    if not ctx.dll_path:
        logger.error(
            "Grandiarchipelago.dll not found. Set GRANDIA_AP_DLL or place the DLL next to "
            "Archipelago / in client/build/Release."
        )

    waiting_for_slot_logged = False

    while not ctx.exit_event.is_set():
        try:
            if ctx.request_reattach:
                ctx.request_reattach = False
                ctx.hook_injected = False
                ctx._injected_pid = None
                ctx._awaiting_launch = True
                ctx._logged_skip_running = False
                if ctx.pipe:
                    ctx.pipe.close()
                    ctx.pipe = None
                ctx.delivery_open = False
                ctx.save_bound = False

            if ctx._pending_sync is not None and not ctx.delivery_open:
                if time.monotonic() >= ctx._delivery_open_at:
                    ctx.delivery_open = True
                    ctx._pending_sync = None
                    logger.info("Post-load delay done — re-applying map keys and catching up AP items.")
                    ctx.reapply_map_keys()
                    ctx.catch_up_items()

            # Live items after delivery is open
            if ctx.delivery_open and ctx.pipe and ctx.pipe.connected:
                ctx.catch_up_items()

            if ctx.pipe and ctx.pipe.connected:
                while True:
                    line = ctx.pipe.poll_line()
                    if line is None:
                        break
                    ctx.handle_pipe_line(line)
                if not ctx.pipe.connected:
                    logger.warning("Game pipe lost.")
                    ctx.hook_injected = False
                await asyncio.sleep(POLL_S)
                continue

            # Do not inject until we are connected to an AP slot.
            if not getattr(ctx, "slot", None):
                if not waiting_for_slot_logged:
                    logger.info("Waiting for Archipelago slot connection before injecting…")
                    waiting_for_slot_logged = True
                await asyncio.sleep(1.0)
                continue
            waiting_for_slot_logged = False

            if not ctx.dll_path:
                await asyncio.sleep(2.0)
                continue

            pid = find_process_id(PROCESS_NAME)
            if not pid:
                # Process gone → next appearance is a real launch.
                ctx._awaiting_launch = True
                ctx._injected_pid = None
                ctx.hook_injected = False
                ctx._logged_skip_running = False
                await asyncio.sleep(1.0)
                continue

            if not ctx.hook_injected:
                # Same PID we already injected: reconnect pipe only (DLL still loaded).
                if ctx._injected_pid == pid:
                    logger.info("Reconnecting pipe to already-injected %s (pid %s)…", PROCESS_NAME, pid)
                elif not ctx._awaiting_launch:
                    if not ctx._logged_skip_running:
                        logger.info(
                            "%s already running (pid %s) — waiting for relaunch or /attach",
                            PROCESS_NAME,
                            pid,
                        )
                        ctx._logged_skip_running = True
                    await asyncio.sleep(POLL_S)
                    continue
                else:
                    logger.info("Found new %s (pid %s) — injecting…", PROCESS_NAME, pid)
                    if not inject_dll(pid, ctx.dll_path):
                        await asyncio.sleep(2.0)
                        continue
                    ctx._injected_pid = pid
                    ctx._awaiting_launch = False
                ctx.hook_injected = True

            pipe = GamePipe()
            if not pipe.connect(timeout_ms=60000):
                ctx.hook_injected = False
                await asyncio.sleep(2.0)
                continue
            ctx.pipe = pipe
            # Ensure seed_hash reaches the DLL even if HELLO raced ahead of Connected.
            ctx._push_runtime_config()
            # Drain HELLO / early lines
            await asyncio.sleep(0.2)
            while True:
                line = pipe.poll_line()
                if line is None:
                    break
                ctx.handle_pipe_line(line)

        except Exception as exc:
            logger.exception("Game watcher error: %s", exc)
            if ctx.pipe:
                ctx.pipe.close()
                ctx.pipe = None
            ctx.hook_injected = False
            await asyncio.sleep(2.0)

        await asyncio.sleep(POLL_S)


async def main(args) -> None:
    # CommonClient runs server_auth during RoomInfo *before* on_package(RoomInfo),
    # so Connected can race with an empty seed_name. Capture seed as soon as RoomInfo arrives.
    import CommonClient as _cc

    _orig_process = _cc.process_server_cmd

    async def _process_server_cmd(ctx, cmd_args: dict):
        if cmd_args.get("cmd") == "RoomInfo" and cmd_args.get("seed_name"):
            ctx.seed_name = cmd_args["seed_name"]
        await _orig_process(ctx, cmd_args)

    _cc.process_server_cmd = _process_server_cmd

    ctx = GrandiaContext(args.connect, args.password)
    if getattr(args, "name", None):
        ctx.auth = args.name
    ctx.server_task = asyncio.create_task(server_loop(ctx), name="server_loop")

    if gui_enabled:
        ctx.run_gui()
    ctx.run_cli()

    watcher = asyncio.create_task(game_watcher(ctx), name="GrandiaGameWatcher")
    await ctx.exit_event.wait()
    ctx.server_address = None
    await ctx.shutdown()
    watcher.cancel()
    try:
        await watcher
    except asyncio.CancelledError:
        pass
    if ctx.pipe:
        ctx.pipe.close()


def launch(*args: str) -> None:
    import colorama

    colorama.init()
    parser = get_base_parser(description="Grandiarchipelago Client")
    parser.add_argument("--name", default=None, help="Slot / player name")
    parsed = parser.parse_known_args(list(args))[0]
    asyncio.run(main(parsed))
    colorama.deinit()


if __name__ == "__main__":
    launch(*sys.argv[1:])
