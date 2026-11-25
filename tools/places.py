"""Build places.json: the rooms, grottos and interiors of every map, for the explorer (src/ootgz/placer.c).

    python places.py              writes ../data/places.json (needs OOTMENU_NUP and zstandard)

Each map layout (romfs/Banc/<Map>/<Layout>_{Static,Dynamic}.bcett.byml.zs) gives:

- rooms (dungeons): each DungeonRoomCenterAnchor, numbered by the room of the placed actors nearest it (their
  Dynamic.DefaultDungeonRoomIndex: the anchors carry no number of their own), at the anchor;
- grottos: each logic group HidingHole_Level_Hole_NN_Prefab (one per grotto; the EnvArea ones are only its look),
  at its DestinationAnchor (where the game puts Link when he drops in) or, with none, among its lowest actors;
- interiors (maps with no rooms): the IndoorAreaBox volumes, the Land_* interior pieces and whole buildings
  (Land_*_House_*, not their Outside shells: a house's insides are in the building where it stands, its origin on
  the floor inside), gathered into places where they're close; at a building's origin, else at the middle across
  and the lowest point up.

A place is labelled by what's in it: a room's or grotto's enemies and chests, an interior's people. The explorer
puts the beam on the ground under a place's point and goes there with load position's move.
"""
import collections
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import byml  # noqa: E402
import gamefiles  # noqa: E402

OUT = os.path.join(os.path.dirname(HERE), "data", "places.json")
SUFFIX = ".bcett.byml.zs"
GROTTO = re.compile(r"HidingHole_Level_Hole_\d+_Prefab")
INTERIOR_PIECE = re.compile(r"^Land_.*(Interior|Inside|Indoor|_House_|WindmillHouse)", re.I)
HOUSE = re.compile(r"^Land_.*(_House_|WindmillHouse)")  # a whole building, its origin on the floor inside
OUTSIDE = re.compile(r"Outside", re.I)
ROOM_ANCHOR_MAX = 40.0            # a room's centre anchor is no further than this from the middle of its actors
SAME_GROTTO = 6.0                 # grotto groups this near across are one grotto (its hole, its room, its look)
LINK_ACROSS, LINK_UP = 7.0, 5.0  # interior pieces this near each other belong to one place
MIN_PIECES = 3                   # fewer: a stray piece, not a place
LABEL_KINDS = 2                  # the kinds of thing a label names

# words for what a place holds (a name's start): (pattern, word); the first that matches
THINGS = [
    (r"^Obj_TBox", "chest"),
    (r"^Npc_GreatFairy|^Obj_FairyFountain|GreatFairy", "great fairy"),
    (r"^FairySpirit|FairyFountain", "fairy fountain"),
    (r"^Enemy_Scrub_Business", "business scrubs"),
    (r"^Obj_GossipStone|^GossipStone", "gossip stone"),
    (r"^Enemy_Skulltula_Gold|Skulltula_Gold", "gold skulltula"),
    (r"^Npc_Cow|_Cow_", "cow"),
]
SKIP_THING = re.compile(r"Leaf$|_AppearPoint|_Dictionary|Larva|Drop|Area|Anchor|Hand$")


RACES = {"Hylia": "hylian", "Kokiri": "kokiri", "Goron": "goron", "Zora": "zora", "Gerudo": "gerudo"}
WORD_FIXES = {"re dead": "redead", "dekubaba": "deku baba", "scrub deku": "deku scrub", "tektite blue": "blue tektite",
              "tektite red": "red tektite", "dungeon boss gohma": "gohma", "keese fire": "fire keese",
              "bubble green": "green bubble", "bubble blue": "blue bubble", "bubble red": "red bubble"}


def words(name):
    """An actor name made readable: 'Enemy_ReDead_AlwaysRespawn' -> 'redead', 'Npc_Hylia_Talon' -> 'talon',
    'Npc_Hylia_024' -> 'hylian'."""
    for pattern, word in THINGS:
        if re.search(pattern, name):
            return word
    parts = name.split("_")[1:] or [name]
    if parts[0] in RACES:  # a race, then a name or a number
        if len(parts) == 1 or re.fullmatch(r"\d+", parts[1]):
            return RACES[parts[0]]
        parts = parts[1:]
    keep = []
    for part in parts:
        if re.fullmatch(r"[A-Z]?\d+|[A-Z]|AlwaysRespawn|NoBake|Adult|Child|Big|Small|Set|Common", part):
            break
        keep.append(part)
    text = " ".join(keep) or parts[0]
    text = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", text).lower()
    for wrong, right in WORD_FIXES.items():
        text = re.sub(rf"\b{wrong}\b", right, text)
    return text


def label_things(actors, kinds=("Enemy_", "Obj_TBox", "Npc_", "FairySpirit", "Obj_GossipStone")):
    counts = collections.Counter()
    for a in actors:
        name = a.get("Gyaml", "")
        if name.startswith(kinds) and not SKIP_THING.search(name) and not name.startswith("Npc_Navi"):
            counts[words(name)] += 1
    out = []
    for word, n in counts.most_common(LABEL_KINDS):
        out.append(f"{word} x{n}" if n > 1 else word)
    return ", ".join(out)


def pos(a):
    return [float(v) for v in (a.get("Translate") or [0, 0, 0])]


def rooms(acts):
    """Every room the placed actors are numbered into, at the centre anchor nearest the middle of its actors (each
    anchor one room's; the anchors carry no number of their own), else at that middle on its lowest actor's floor."""
    anchors = [pos(a) for a in acts if a.get("Gyaml") == "DungeonRoomCenterAnchor"]
    numbered = [a for a in acts if isinstance((a.get("Dynamic") or {}).get("DefaultDungeonRoomIndex"), int)]
    if not anchors or not numbered:
        return []
    by_room = collections.defaultdict(list)
    for a in numbered:
        by_room[a["Dynamic"]["DefaultDungeonRoomIndex"]].append(a)
    middles = {}
    for room, members in by_room.items():
        ps = [pos(a) for a in members]
        middles[room] = [sum(v[i] for v in ps) / len(ps) for i in range(3)]
        middles[room][1] = min(v[1] for v in ps)
    # nearest pairs first, each anchor and each room once
    pairs = sorted((math.dist(anchor, middles[room]), i, room) for i, anchor in enumerate(anchors) for room in middles)
    at, used = {}, set()
    for d, i, room in pairs:
        if room in at or i in used or d > ROOM_ANCHOR_MAX:
            continue
        at[room] = anchors[i]
        used.add(i)
    places = []
    for room in sorted(by_room):
        things = label_things(by_room[room], kinds=("Enemy_", "Obj_TBox"))
        places.append({"kind": "room", "n": room, "label": f"room {room}" + (f": {things}" if things else ""),
                       "pos": at.get(room, middles[room])})
    return places


def grottos(acts_by_hash, groups):
    places = []
    for g in groups:
        if not GROTTO.search(g.get("Logic", "")):
            continue
        members = [acts_by_hash[r.get("Reference")] for r in g.get("References") or [] if r.get("Reference") in acts_by_hash]
        dest = [a for a in members if a.get("Gyaml") == "DestinationAnchor"]
        solid = [a for a in members if "Area" not in a.get("Gyaml", "") and "Anchor" not in a.get("Gyaml", "")]
        if dest:
            p = pos(dest[0])
        elif solid:
            low = min(pos(a)[1] for a in solid)
            floor = [pos(a) for a in solid if pos(a)[1] < low + 3]  # its lowest actors: the grotto's floor
            p = [sum(v[i] for v in floor) / len(floor) for i in range(3)]
        else:
            continue
        places.append({"kind": "grotto", "pos": p, "dest": bool(dest), "members": members})
    merged = []  # one grotto's groups: its landing point if one has it, else the lowest; all its things
    for place in sorted(places, key=lambda q: (not q["dest"], q["pos"][1])):
        same = next((m for m in merged if math.dist(m["pos"][::2], place["pos"][::2]) < SAME_GROTTO), None)
        if same:
            same["members"] += place["members"]
        else:
            merged.append(place)
    out = []
    for place in merged:
        things = label_things(place["members"])
        out.append({"kind": "grotto", "label": "grotto" + (f": {things}" if things else ""), "pos": place["pos"]})
    return sorted(out, key=lambda q: (q["pos"][0], q["pos"][2]))


def interiors(acts):
    points = [a for a in acts if a.get("Gyaml") == "IndoorAreaBox" or
              (INTERIOR_PIECE.search(a.get("Gyaml", "")) and not OUTSIDE.search(a.get("Gyaml", "")))]
    group = list(range(len(points)))  # union-find over pieces close to each other

    def root(i):
        while group[i] != i:
            group[i] = group[group[i]]
            i = group[i]
        return i

    ps = [pos(a) for a in points]
    for i in range(len(ps)):
        for j in range(i + 1, len(ps)):
            if math.dist(ps[i][::2], ps[j][::2]) < LINK_ACROSS and abs(ps[i][1] - ps[j][1]) < LINK_UP:
                group[root(i)] = root(j)
    clusters = collections.defaultdict(list)
    houses = collections.defaultdict(list)
    for i in range(len(ps)):
        clusters[root(i)].append(ps[i])
        if HOUSE.search(points[i].get("Gyaml", "")) or points[i].get("Gyaml") == "IndoorAreaBox":
            houses[root(i)].append(ps[i])
    npcs = [a for a in acts if a.get("Gyaml", "").startswith("Npc_") and not a["Gyaml"].startswith("Npc_Navi")]
    places = []
    for key, cluster in clusters.items():
        if len(cluster) < MIN_PIECES and not houses[key]:
            continue
        lo = [min(v[i] for v in cluster) for i in range(3)]
        hi = [max(v[i] for v in cluster) for i in range(3)]
        inside = [a for a in npcs if all(lo[i] - 2 <= pos(a)[i] <= hi[i] + 3 for i in range(3))]
        things = label_things(inside, kinds=("Npc_",))
        at = min(houses[key], key=lambda v: v[1]) if houses[key] else [(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2]
        places.append({"kind": "interior", "label": "interior" + (f": {things}" if things else ""), "pos": at})
    return sorted(places, key=lambda place: (place["pos"][0], place["pos"][2]))


def main():
    nup = gamefiles.nup()
    layouts = collections.defaultdict(list)
    for path in nup.index:
        if path.startswith("romfs/Banc/") and path.endswith(SUFFIX):
            stem = path.rsplit("/", 1)[1][:-len(SUFFIX)]
            base, part = stem.rsplit("_", 1)
            if part in ("Static", "Dynamic"):
                layouts[base].append(path)
    maps = {}
    for base in sorted(layouts):
        acts_by_hash, groups = {}, []
        for path in sorted(layouts[base]):
            try:
                root = byml.load(gamefiles.read(path))
            except Exception as e:  # a file this reader can't handle: report and go on
                print(f"skip {path}: {e}")
                continue
            for a in root.get("Actors") or []:
                acts_by_hash[a.get("Hash")] = a
            groups += root.get("AiGroups") or []
        acts = list(acts_by_hash.values())
        found = rooms(acts)
        found += grottos(acts_by_hash, groups)
        if not any(p["kind"] == "room" for p in found):
            found += interiors(acts)
        for p in found:
            p["pos"] = [round(v, 3) for v in p["pos"]]
        if found:
            maps[base] = found
            kinds = collections.Counter(p["kind"] for p in found)
            print(f"{base}: " + ", ".join(f"{n} {k}s" for k, n in sorted(kinds.items())))
    with open(OUT, "w") as f:
        json.dump({"source": os.path.basename(nupfs_path()), "maps": maps}, f, indent=1)
    print(f"wrote {OUT}: {sum(len(v) for v in maps.values())} places in {len(maps)} maps")


def nupfs_path():
    import nupfs
    return nupfs.NUP


if __name__ == "__main__":
    main()
