# Grandiarchipelago

Live **Archipelago multiworld** for **Grandia HD Remaster (PC)**.

Grandiarchipelago hooks the running **32-bit native** game and sends/receives checks over the AP server.

## How to use

- Install the latest release's APWorld
- Launch the Grandia Client from the Archipelago Launcher
- Connect to your slot
- Launch the Game

**Quality of life Shortcuts:**

Right Ctrl (Keyboard) / Select + L1 (Controller) : Toggle SpeedHack
F8 (Keyboard) / Select + R1 (Controller) : Toggle enemy encounters ON/OFF
Backspace (Keyboard) / Select (Controller) : Skip current video cinematic

## Yaml Options

- include_soldiers_graveyard : Include Soldier's Graveyard chest pickups as Archipelago locations.
- include_castle_of_dreams : Include Castle of Dreams chest pickups as Archipelago locations.
- include_tower_of_temptation : Include Tower of Temptation chest pickups as Archipelago locations.
- magic_xp_multiplier
- skill_xp_multiplier
- level_xp_multiplier
- gameplay_balance : Choose between the Vanilla or Redux gameplay tables (stats, enemies, items, shops, names).

## Engine facts (important)

Grandia HD Remaster is a native port (Sickhead Games) built from PlayStation source code, using **SDL + Direct3D**.

## Repository layout

```
Grandiarchipelago/
├── worlds/grandia/                         # Archipelago APWorld (Python)
├── data/                                   # Extracted and mapped Chests / Events
└── tools/
    ├── sync_apworld_from_mdp_catalog.py
    ├── sync_progressions.py
    └── build_grandia_apworld.py
```

## Architecture

```mermaid
flowchart LR
  APWorld[worlds/grandia APWorld] --> Server[Archipelago Server]
  DLL[Grandiarchipelago.dll Win32] <-->|WebSocket| Server
  Game[grandia.exe 32-bit] <-- hooks --> DLL
  Client -->|inject| DLL
```

## Status

| Milestone | State |
|-----------|-------|
| APWorld scaffold | Done |
| **Chest locations from MDP catalog (v1)** | **Done** — ~800 checks, id `0x47522000+event` |
| Item pool filler/useful from vanilla chests | Done |
| Chests event hooks | Done |
| Story Event hooks | Done |
| Gold hooks | Done |
| Gate hooks | Done |
| StoryEvents / progression logic | Done (needs testing) |
| AP network client | Done |
| On the fly Redux | Done |

Sync catalog / progression → APWorld after regenerating MDP data:

```powershell
python tools/sync_progressions.py
python tools/sync_apworld_from_mdp_catalog.py
cmake --build client/build --config Release --target Grandiarchipelago
python build_grandia_apworld.py
```

# Credits

JBDCE : Grandia Remastered Redux (https://github.com/JBDCE/grandiaRemasteredRedux)