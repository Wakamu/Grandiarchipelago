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
from typing import Optional, Set

from CommonClient import ClientCommandProcessor, CommonContext, get_base_parser, gui_enabled, logger, server_loop
from Utils import async_start

from .dll_inject import find_process_id, inject_dll
from .game_pipe import GamePipe

ITEM_DELIVERY_DELAY_S = 2.5
PROCESS_NAME = "grandia.exe"
POLL_S = 0.25


def _package_native_dll_bytes() -> Optional[bytes]:
    """Read bundled native/Grandiarchipelago.dll from the installed world (folder or .apworld)."""
    # Filesystem next to this module (source tree or extracted world folder).
    fs_path = Path(__file__).resolve().parent / "native" / "Grandiarchipelago.dll"
    if fs_path.is_file():
        return fs_path.read_bytes()

    try:
        from importlib.resources import files

        resource = files(__package__).joinpath("native/Grandiarchipelago.dll")
        return resource.read_bytes()
    except Exception:
        return None


def _ensure_bundled_dll() -> Optional[Path]:
    """
    Prefer the DLL shipped inside the APWorld. LoadLibrary needs a real file path,
    so when the world is a .apworld zip we extract to the Archipelago user cache.
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

    try:
        from Utils import user_path

        cache_dir = Path(user_path("Grandia", "native"))
    except Exception:
        cache_dir = Path.home() / "Archipelago" / "Grandia" / "native"

    cache_dir.mkdir(parents=True, exist_ok=True)
    dest = cache_dir / "Grandiarchipelago.dll"
    if not dest.is_file() or dest.stat().st_size != len(data):
        dest.write_bytes(data)
        logger.info("Extracted bundled inject DLL to %s", dest)
    return dest.resolve()


def _resolve_dll_path() -> Optional[Path]:
    env = os.environ.get("GRANDIA_AP_DLL")
    if env:
        p = Path(env)
        if p.is_file():
            return p.resolve()

    bundled = _ensure_bundled_dll()
    if bundled is not None:
        return bundled

    candidates = [
        Path(__file__).resolve().parents[2] / "client" / "build" / "Release" / "Grandiarchipelago.dll",
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
        self.last_sync_index: Optional[int] = None
        self.applied_index = 0
        self.delivery_open = False
        self._delivery_open_at = 0.0
        self._forwarded_indexes: Set[int] = set()
        self._pending_sync: Optional[int] = None

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
        if cmd == "Connected":
            logger.info("Connected to Archipelago as %s", self.auth)
            logger.info("Holding item delivery until the game sends SYNC from a loaded save.")
        elif cmd == "RoomInfo":
            pass

    def apply_sync(self, received_index: int) -> None:
        self._forwarded_indexes = {i for i in self._forwarded_indexes if i <= received_index}
        self.applied_index = received_index
        self.last_sync_index = received_index
        self.delivery_open = False
        self._pending_sync = received_index
        self._delivery_open_at = time.monotonic() + ITEM_DELIVERY_DELAY_S
        logger.info(
            "Save SYNC received_index=%s — delivering Index > %s after %.1fs",
            received_index,
            received_index,
            ITEM_DELIVERY_DELAY_S,
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
        name = self.item_names.lookup_in_game(item_id) if hasattr(self, "item_names") else str(item_id)
        self.pipe.send_line(f"ITEM 0x{item_id:X} INDEX {index}")
        self._forwarded_indexes.add(index)
        self.applied_index = index
        logger.info("Forwarded item %s (0x%X) INDEX %s", name, item_id, index)

    def handle_pipe_line(self, line: str) -> None:
        if line.startswith("HELLO"):
            logger.info("Game said: %s", line)
            self.pipe and self.pipe.send_line("CONNECTED")
            return
        if line.startswith("SYNC "):
            token = line[5:].strip()
            try:
                index = int(token)
            except ValueError:
                logger.warning("Invalid SYNC line: %s", line)
                return
            if index < 0:
                return
            self.apply_sync(index)
            return
        if line.startswith("CHECK "):
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
            logger.info("Reported location check: 0x%X", location_id)
            return
        logger.info("Game pipe: %s", line)


async def game_watcher(ctx: GrandiaContext) -> None:
    logger.info("Game watcher started — launch Grandia past the Steam menu.")
    if not ctx.dll_path:
        logger.error(
            "Grandiarchipelago.dll not found. Set GRANDIA_AP_DLL or place the DLL next to "
            "Archipelago / in client/build/Release."
        )

    while not ctx.exit_event.is_set():
        try:
            if ctx.request_reattach:
                ctx.request_reattach = False
                ctx.hook_injected = False
                if ctx.pipe:
                    ctx.pipe.close()
                    ctx.pipe = None
                ctx.delivery_open = False

            if ctx._pending_sync is not None and not ctx.delivery_open:
                if time.monotonic() >= ctx._delivery_open_at:
                    ctx.delivery_open = True
                    ctx._pending_sync = None
                    logger.info("Post-load delay done — catching up AP items.")
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

            if not ctx.dll_path:
                await asyncio.sleep(2.0)
                continue

            pid = find_process_id(PROCESS_NAME)
            if not pid:
                await asyncio.sleep(1.0)
                continue

            if not ctx.hook_injected:
                logger.info("Found %s (pid %s) — injecting…", PROCESS_NAME, pid)
                if inject_dll(pid, ctx.dll_path):
                    ctx.hook_injected = True
                else:
                    await asyncio.sleep(2.0)
                    continue

            pipe = GamePipe()
            if not pipe.connect(timeout_ms=60000):
                ctx.hook_injected = False
                await asyncio.sleep(2.0)
                continue
            ctx.pipe = pipe
            # Drain HELLO
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
