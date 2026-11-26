"""The paths NPCs walk (guard vision's patrol paths): each map layout's rails named "Guide", to data/rails.json.

    python tools/rails.py

A layout (romfs/Banc/<Map>/<Layout>_{Static,Dynamic}.bcett.byml.zs) keeps its rails under "Rails": each has
IsClosed and Points, a point a Translate, a Rotate (radians; Y the way an NPC faces there when it adjusts to the
point) and Dynamic (WaitFrame: how long an NPC stops there, WaitASKeyName: what it does meanwhile, Act_LookAround
for the guards). The guards' patrol (ActionSeq_RailMoveToDefault, NpcCommon_Patrol) walks a Guide rail, back and
forth or round a closed one (FINDINGS 9.36). Other rails (SoundControl, unnamed ones) aren't kept.
Who walks one: an actor's own "Rails" list links it to its rail (Dst: the rail's Hash, Name "Reference"); its
"owners" here are those actors' Hash (the instance id the game keeps on the actor) and name, from any layout of the map
(177 of the 182 have one: guards, soldiers, townspeople, dogs, cuccos, and moving platforms and enemies).

Set OOTMENU_NUP to the dump's path (see nupfs.py). Needs zstandard.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import byml  # noqa: E402
import gamefiles  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "rails.json")
LAYOUT_SUFFIX = ".bcett.byml.zs"
KEPT = ("Guide",)


def main():
    nup = gamefiles.nup()
    layouts = sorted(path for path in nup.index if path.startswith("romfs/Banc/") and path.endswith(LAYOUT_SUFFIX))
    maps, found = {}, {}  # found: map -> ([(rail hash, rail)], {rail hash: [(actor hash, name)]})
    for path in layouts:
        stem = path.rsplit("/", 1)[1][:-len(LAYOUT_SUFFIX)]
        part = stem.rsplit("_", 1)[-1]
        map_name = stem.rsplit("_", 1)[0] if part in ("Static", "Dynamic") else stem
        try:
            root = byml.load(gamefiles.read(path))
        except Exception as e:  # a file this reader can't handle: report and go on
            print(f"skip {path}: {e}")
            continue
        rails, owners = found.setdefault(map_name, ([], {}))
        for rail in root.get("Rails") or []:
            if rail.get("Gyaml") not in KEPT:
                continue
            points = []
            for point in rail.get("Points") or []:
                t = [round(float(v), 3) for v in point.get("Translate", [0, 0, 0])]
                yaw = round(float((point.get("Rotate") or [0, 0, 0])[1]), 4)
                wait = float((point.get("Dynamic") or {}).get("WaitFrame", 0))
                points.append(t + [yaw, round(wait, 1)])
            if len(points) >= 2:
                rails.append((rail.get("Hash"), {"closed": bool(rail.get("IsClosed")), "points": points}))
        for actor in root.get("Actors") or []:
            for link in actor.get("Rails") or []:
                if actor.get("Hash") is not None and link.get("Dst") is not None:
                    owners.setdefault(link["Dst"], []).append([str(actor["Hash"]), actor.get("Gyaml", "")])
    owned = 0
    for map_name, (rails, owners) in sorted(found.items()):
        for rail_hash, rail in rails:
            rail["owners"] = owners.get(rail_hash, [])
            owned += bool(rail["owners"])
            maps.setdefault(map_name, []).append(rail)
    with open(OUT, "w") as f:
        json.dump({"source": "game files (tools/rails.py)", "maps": maps}, f, separators=(",", ":"))
    total = sum(len(rails) for rails in maps.values())
    print(f"{total} guide rails in {len(maps)} maps ({owned} with an actor that walks it) -> {OUT}")


if __name__ == "__main__":
    main()
