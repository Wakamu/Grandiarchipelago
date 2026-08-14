#!/usr/bin/env python3
"""Interactive progression / map accessibility viewer.

Reads data/player_maps/progressions.json + data/mdp_chest_catalog.json,
writes a self-contained HTML page, and opens it in the default browser.

Usage (repo root):
  python tools/progression_map_viewer.py
  python tools/progression_map_viewer.py --no-open
"""

from __future__ import annotations

import argparse
import json
import webbrowser
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PROGRESSIONS = REPO / "data" / "player_maps" / "progressions.json"
CATALOG = REPO / "data" / "mdp_chest_catalog.json"
OUT_HTML = REPO / "tools" / "progression_map_viewer.html"


def map_names_from_catalog(catalog: dict) -> dict[int, str]:
    """map_id (int) -> best area_name from MDP catalog."""
    names: dict[int, str] = {}
    for entry in catalog.values():
        mid = int(entry["map_id"])
        name = (entry.get("area_name") or "").strip()
        if name and mid not in names:
            names[mid] = name
    return names


def all_progression_maps(prog: dict) -> set[int]:
    maps: set[int] = set(int(x) for x in prog.get("start_maps", []))
    for ev in prog.get("events", []):
        maps.add(int(ev["map"]))
        if "key" in ev:
            maps.update(int(x) for x in ev["key"]["unlocks_maps"])
        if "blocks" in ev:
            maps.update(int(x) for x in ev["blocks"])
    return maps


HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<title>Progression map viewer</title>
<style>
  :root {
    --bg: #1a1b1e;
    --panel: #25262b;
    --border: #373a40;
    --text: #e9ecef;
    --muted: #909296;
    --accent: #4dabf7;
    --ok: #69db7c;
    --warn: #ffa94d;
    --blocked: #ff6b6b;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    font: 14px/1.45 system-ui, Segoe UI, sans-serif;
    background: var(--bg);
    color: var(--text);
  }
  header {
    padding: 16px 20px;
    border-bottom: 1px solid var(--border);
    display: flex;
    flex-wrap: wrap;
    gap: 12px 24px;
    align-items: baseline;
  }
  header h1 { margin: 0; font-size: 18px; font-weight: 600; }
  header .meta { color: var(--muted); font-size: 12px; }
  header .actions { margin-left: auto; display: flex; gap: 8px; }
  button {
    background: var(--panel);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 6px 12px;
    cursor: pointer;
  }
  button:hover { border-color: var(--accent); }
  main {
    display: grid;
    grid-template-columns: minmax(320px, 1fr) minmax(320px, 1fr);
    gap: 0;
    min-height: calc(100vh - 58px);
  }
  @media (max-width: 900px) {
    main { grid-template-columns: 1fr; }
  }
  section {
    padding: 16px 20px;
    overflow: auto;
    max-height: calc(100vh - 58px);
  }
  section + section { border-left: 1px solid var(--border); }
  h2 {
    margin: 0 0 12px;
    font-size: 13px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--muted);
    font-weight: 600;
  }
  .stats {
    display: flex;
    gap: 16px;
    margin-bottom: 14px;
    flex-wrap: wrap;
  }
  .stat {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 8px 12px;
    min-width: 88px;
  }
  .stat b { display: block; font-size: 20px; }
  .stat span { color: var(--muted); font-size: 11px; }
  .event {
    display: flex;
    gap: 10px;
    align-items: flex-start;
    padding: 10px 12px;
    margin-bottom: 6px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
  }
  .event.unreachable { opacity: 0.45; }
  .event input { margin-top: 3px; }
  .event .body { flex: 1; min-width: 0; }
  .event .name { font-weight: 600; }
  .event .detail { color: var(--muted); font-size: 12px; margin-top: 2px; }
  .tag {
    display: inline-block;
    font-size: 11px;
    padding: 1px 6px;
    border-radius: 4px;
    border: 1px solid var(--border);
    margin-right: 4px;
    color: var(--muted);
  }
  .tag.key { color: var(--accent); border-color: #364fc7; }
  .tag.blocks { color: var(--blocked); border-color: #c92a2a; }
  .tag.gold { color: var(--warn); }
  .tag.finish { color: var(--ok); }
  .map-list { list-style: none; margin: 0; padding: 0; }
  .map-list li {
    display: flex;
    justify-content: space-between;
    gap: 12px;
    padding: 8px 12px;
    border-bottom: 1px solid var(--border);
  }
  .map-list li:hover { background: var(--panel); }
  .map-name { font-weight: 500; }
  .map-id { color: var(--muted); font-family: ui-monospace, Consolas, monospace; font-size: 12px; white-space: nowrap; }
  .map-source { color: var(--muted); font-size: 11px; }
  .group-title {
    margin: 16px 0 6px;
    font-size: 12px;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .hint { color: var(--muted); font-size: 12px; margin-bottom: 12px; }
</style>
</head>
<body>
<header>
  <h1>Progression map viewer</h1>
  <div class="meta">Source: progressions.json · names from mdp_chest_catalog.json</div>
  <div class="actions">
    <button type="button" id="btn-none">Uncheck all</button>
    <button type="button" id="btn-all">Check all</button>
  </div>
</header>
<main>
  <section>
    <h2>Story events</h2>
    <p class="hint">Check events to simulate completion. Available maps = start maps + key unlocks (a key with value N only unlocks after all keys with value 1..N are obtained) − blocks from checked events.</p>
    <div class="stats">
      <div class="stat"><b id="stat-checked">0</b><span>events checked</span></div>
      <div class="stat"><b id="stat-maps">0</b><span>maps available</span></div>
      <div class="stat"><b id="stat-blocked">0</b><span>maps blocked</span></div>
    </div>
    <div id="events"></div>
  </section>
  <section>
    <h2>Available maps</h2>
    <div id="maps"></div>
  </section>
</main>
<script>
const DATA = __DATA__;

const mapName = (id) => DATA.names[String(id)] || null;
const fmtMap = (id) => {
  const hex = id.toString(16).toUpperCase().padStart(4, "0");
  const name = mapName(id);
  return { id, hex, name, label: name || `Map ${hex}` };
};

function compute(checkedIds) {
  const checked = new Set(checkedIds);
  const available = new Set(DATA.start_maps);
  const sources = new Map(); // mapId -> reason strings
  for (const id of DATA.start_maps) {
    sources.set(id, ["start"]);
  }

  // Keys granted by checked story events.
  const ownedKeys = DATA.events.filter((ev) => checked.has(ev.id) && ev.key_value != null);
  const hasAllUpTo = (value) => {
    const needed = DATA.events.filter((ev) => ev.key_value != null && ev.key_value <= value);
    return needed.every((ev) => checked.has(ev.id));
  };

  for (const ev of ownedKeys) {
    if (!hasAllUpTo(ev.key_value)) continue;
    if (!ev.unlocks || !ev.unlocks.length) continue;
    const reason = `${ev.key_name} (v${ev.key_value})`;
    for (const mid of ev.unlocks) {
      available.add(mid);
      const s = sources.get(mid) || [];
      if (!s.includes(reason)) s.push(reason);
      sources.set(mid, s);
    }
  }

  const blocked = new Set();
  const blockedBy = new Map();
  for (const ev of DATA.events) {
    if (!checked.has(ev.id) || !ev.blocks) continue;
    for (const mid of ev.blocks) {
      blocked.add(mid);
      available.delete(mid);
      const s = blockedBy.get(mid) || [];
      s.push(ev.name);
      blockedBy.set(mid, s);
    }
  }
  return { available, blocked, sources, blockedBy };
}

function eventReachable(ev, available) {
  // Event can fire if its map is currently available (or was when unlocked).
  // Soft hint only — authoring aid, not full AP reachability.
  return available.has(ev.map);
}

function render() {
  const checked = [...document.querySelectorAll("#events input[type=checkbox]:checked")]
    .map((el) => Number(el.value));
  const { available, blocked, sources, blockedBy } = compute(checked);

  document.getElementById("stat-checked").textContent = String(checked.length);
  document.getElementById("stat-maps").textContent = String(available.size);
  document.getElementById("stat-blocked").textContent = String(blocked.size);

  // Refresh unreachable styling on events
  for (const ev of DATA.events) {
    const row = document.getElementById(`ev-${ev.id}`);
    if (!row) continue;
    const box = row.querySelector("input");
    const softUnreachable = !box.checked && !eventReachable(ev, available);
    row.classList.toggle("unreachable", softUnreachable);
  }

  const mapsEl = document.getElementById("maps");
  const availList = [...available].sort((a, b) => a - b).map(fmtMap);
  const blockedList = [...blocked].sort((a, b) => a - b).map(fmtMap);

  let html = "";
  if (availList.length) {
    html += `<div class="group-title">Reachable (${availList.length})</div><ul class="map-list">`;
    for (const m of availList) {
      const why = (sources.get(m.id) || []).join(", ");
      html += `<li>
        <div>
          <div class="map-name">${escapeHtml(m.label)}</div>
          <div class="map-source">${escapeHtml(why)}</div>
        </div>
        <div class="map-id">0x${m.hex} · ${m.id}</div>
      </li>`;
    }
    html += "</ul>";
  }
  if (blockedList.length) {
    html += `<div class="group-title">Blocked by checked events (${blockedList.length})</div><ul class="map-list">`;
    for (const m of blockedList) {
      const why = (blockedBy.get(m.id) || []).join(", ");
      html += `<li>
        <div>
          <div class="map-name" style="color:var(--blocked)">${escapeHtml(m.label)}</div>
          <div class="map-source">${escapeHtml(why)}</div>
        </div>
        <div class="map-id">0x${m.hex} · ${m.id}</div>
      </li>`;
    }
    html += "</ul>";
  }
  mapsEl.innerHTML = html;
}

function escapeHtml(s) {
  return String(s)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function buildEvents() {
  const root = document.getElementById("events");
  root.innerHTML = "";
  for (const ev of DATA.events) {
    const tags = [];
    if (ev.key_name) tags.push(`<span class="tag key">${escapeHtml(ev.key_name)} · v${ev.key_value}</span>`);
    if (ev.blocks && ev.blocks.length) tags.push(`<span class="tag blocks">blocks ${ev.blocks.length}</span>`);
    if (ev.gold != null) tags.push(`<span class="tag gold">gold ${ev.gold}</span>`);
    if (ev.finish) tags.push(`<span class="tag finish">finish</span>`);
    const map = fmtMap(ev.map);
    const row = document.createElement("label");
    row.className = "event";
    row.id = `ev-${ev.id}`;
    row.innerHTML = `
      <input type="checkbox" value="${ev.id}"/>
      <div class="body">
        <div class="name">${escapeHtml(ev.name)}</div>
        <div class="detail">id ${ev.id} · on ${escapeHtml(map.label)} (0x${map.hex})</div>
        <div class="detail">${tags.join(" ")}</div>
      </div>`;
    row.querySelector("input").addEventListener("change", render);
    root.appendChild(row);
  }
}

document.getElementById("btn-none").addEventListener("click", () => {
  for (const el of document.querySelectorAll("#events input")) el.checked = false;
  render();
});
document.getElementById("btn-all").addEventListener("click", () => {
  for (const el of document.querySelectorAll("#events input")) el.checked = true;
  render();
});

buildEvents();
render();
</script>
</body>
</html>
"""


def build_payload(prog: dict, names: dict[int, str]) -> dict:
    events = []
    for ev in prog.get("events", []):
        entry = {
            "name": ev["name"],
            "id": int(ev["id"]),
            "map": int(ev["map"]),
            "gold": int(ev["gold"]) if "gold" in ev else None,
            "finish": bool(ev.get("finish")),
            "blocks": [int(x) for x in ev.get("blocks", [])] or None,
            "key_name": None,
            "key_value": None,
            "unlocks": None,
        }
        if "key" in ev:
            entry["key_name"] = str(ev["key"]["name"])
            entry["key_value"] = int(ev["key"]["value"])
            entry["unlocks"] = [int(x) for x in ev["key"]["unlocks_maps"]]
        events.append(entry)
    return {
        "start_maps": [int(x) for x in prog.get("start_maps", [])],
        "events": events,
        "names": {str(k): v for k, v in names.items()},
        "scope_maps": sorted(all_progression_maps(prog)),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-open", action="store_true", help="Write HTML only, do not open browser")
    args = parser.parse_args()

    prog = json.loads(PROGRESSIONS.read_text(encoding="utf-8"))
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    names = map_names_from_catalog(catalog)
    payload = build_payload(prog, names)

    html = HTML_TEMPLATE.replace("__DATA__", json.dumps(payload, ensure_ascii=False))
    OUT_HTML.write_text(html, encoding="utf-8")
    print(f"Wrote {OUT_HTML.relative_to(REPO)}")
    print(f"  events={len(payload['events'])} start_maps={len(payload['start_maps'])} named={len(names)}")

    if not args.no_open:
        webbrowser.open(OUT_HTML.resolve().as_uri())


if __name__ == "__main__":
    main()
