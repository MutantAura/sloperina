"""Where Link can talk to or otherwise act on each actor (interaction ranges): its attention clients, to
data/interactions.json.

    python tools/interactions.py

An actor's ActorParam (Actor/<name>.engine__actor__ActorParam.bgyml, with its $parent chain) names an
AttentionSenderParam (Components.AttentionSenderRef), whose ClientList names attention clients
(Attention/ClientParam/*.game__attention__ClientParam.bgyml, in the actor's pack or ResidentCommon.pack). A client has
an ActionType (what A does: Talk, Check, OpenDoor, OpenTreasureBox, Lift... MurmurHash3 of the name), an AttentionType
(Action, or LockOn for one that only counts while Z-targeting) and a CheckList, every check of which must pass; the
area checks give the shape, chosen by CheckType (the MurmurHash3 of the block's name after "CheckType"):
  AreaFan          AttentionFanRadius / Angle / Top / Bottom, Margin*: the extra once it's Link's attention
  AreaCylinderFan  AttentionFanRadius / Angle, AttentionCylinderRadius
  AreaSphere       AttentionRadius
  AreaBox          a box in the actor's own axes (+X its matrix's column 0, +Z its front): its model box grown by
                   AttentionMin on its negative sides and AttentionMax on its positive ones, moved by Offset
IsAreaExpandFromPlayer: the area is Link's (a fan in front of him that the actor has to be in, within Angle either way
of his facing; not checked while a lock-on holds with IsAngleCheckIgnoreWhileLockOn), else the actor's own (in front of
it, turned with it). Several area checks in one client must all pass: kept as they are.

The model box is the actor's own, live: its bounds at actor+0x3B8 (min) / +0x3C4 (max), its whole model (a broad
shopkeeper's is 3.2 m across), unless ForceEditModelArea gives ModelMin / ModelMax; IsIgnoreClientSize leaves it out.
A fan's reach adds the box's widest half (the largest of its x and z bounds, either way) to the radius. So it can't be
read from the files: "sized" areas get it added in game (interactview.c), and a box's "min" / "max" here are only its
growth (-AttentionMin + Offset, AttentionMax + Offset), with ModelMin / ModelMax added in when forced.
Its margin, once it has Link's attention: Margin* with ForceEditMargin, else AutoMarginLockOn for a lock-on client and
AutoMarginDefault (0.1) for the rest. All read from the game's checks (main+0x2C565D0 box, main+0x2C54430 fan, 1.1.1)
and measured round a shopkeeper (FINDINGS 9.37).
Set OOTMENU_NUP to the dump's path (see nupfs.py). Needs zstandard. Takes some minutes (every actor pack).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import byml  # noqa: E402
import gamefiles  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "interactions.json")
PACK_PREFIX, PACK_SUFFIX = "romfs/Pack/Actor/", ".pack.zs"
# the ActionType and AttentionType names (found by hashing main's strings)
ACTION_NAMES = ["Talk", "Check", "Grab", "Lift", "Take", "Ride", "OpenDoor", "OpenTreasureBox", "OpenShutter",
                "OpenChainLock", "OpenForEvent", "BringAlong", "Touch", "PullOut", "PutIn", "UseBeansHole", "GrabSword",
                "DiggingHoleTour", "BlueFishTarget", "BlueFireTarget", "None", "Awake", "Action", "LockOn"]
AREAS = ("AreaFan", "AreaCylinderFan", "AreaSphere", "AreaBox")


def mm3(text):
    """MurmurHash3 x86 32, seed 0 (the game's name hashes)."""
    data = text.encode()
    c1, c2, h, n = 0xCC9E2D51, 0x1B873593, 0, len(data)
    for i in range(0, n // 4 * 4, 4):
        k = int.from_bytes(data[i:i + 4], "little")
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        h ^= (k * c2) & 0xFFFFFFFF
        h = ((h << 13) | (h >> 19)) & 0xFFFFFFFF
        h = (h * 5 + 0xE6546B64) & 0xFFFFFFFF
    k = 0
    for j, b in enumerate(data[n // 4 * 4:]):
        k |= b << (8 * j)
    if n % 4:
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        h ^= (k * c2) & 0xFFFFFFFF
    h ^= n
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    return h ^ (h >> 16)


NAME_OF = {mm3(name): name for name in ACTION_NAMES}
AREA_OF = {mm3(name): name for name in AREAS}


class Loader:
    def __init__(self, files, resident):
        self.files, self.resident = files, resident

    def raw(self, ref):
        path = ref.lstrip("?")
        if path.startswith("Work/"):
            path = path[5:]
        path = path.replace(".gyml", ".bgyml")
        for files in (self.files, self.resident):
            if path in files:
                return byml.load(files[path])
        return None

    def load(self, ref, depth=0):
        """The file with its $parent chain merged under it (dicts key by key, anything else replaced)."""
        data = self.raw(ref)
        if not isinstance(data, dict) or depth > 8 or not data.get("$parent"):
            return data
        return merge(self.load(data["$parent"], depth + 1) or {}, data)


def merge(base, over):
    if isinstance(base, dict) and isinstance(over, dict):
        out = dict(base)
        for key, value in over.items():
            if key != "$parent":
                out[key] = merge(base[key], value) if key in base else value
        return out
    return over


def f(value):
    return round(float(value or 0), 3)


def xz_max(*vectors):
    """The largest x or z of these margins (a bare number in some files: that)."""
    return max(f(v.get(a)) if isinstance(v, dict) else f(v) for v in vectors for a in "XZ")


def area(check, lock_on):
    name = AREA_OF.get(check.get("CheckType"))
    block = check.get("CheckType" + name) if name else None
    if not block:
        return None
    forced = bool(block.get("ForceEditMargin"))
    auto = "AutoMarginLockOn" if lock_on else "AutoMarginDefault"
    out = {"shape": name[4:].lower(), "from_link": bool(block.get("IsAreaExpandFromPlayer")),
           "sized": not block.get("IsIgnoreClientSize")}
    if name in ("AreaFan", "AreaCylinderFan"):
        out.update(radius=f(block.get("AttentionFanRadius")), angle=f(block.get("AttentionFanAngle")),
                   margin=f(block.get("MarginFanRadius")) if forced else xz_max(block.get(auto) or 0))
    if name == "AreaFan":
        out.update(top=f(block.get("AttentionFanTop")), bottom=f(block.get("AttentionFanBottom")))
    if name == "AreaCylinderFan":
        out.update(cylinder=f(block.get("AttentionCylinderRadius")))
    if name == "AreaSphere":
        out.update(radius=f(block.get("AttentionRadius")),
                   margin=f(block.get("MarginRadius")) if forced else xz_max(block.get(auto) or 0))
    if name == "AreaBox":
        grow_lo, grow_hi = block.get("AttentionMin") or {}, block.get("AttentionMax") or {}
        offset = block.get("Offset") or {}
        lo = [f(offset.get(a)) - f(grow_lo.get(a)) for a in "XYZ"]
        hi = [f(offset.get(a)) + f(grow_hi.get(a)) for a in "XYZ"]
        if block.get("ForceEditModelArea"):  # its own model box, not the actor's
            m_lo, m_hi = block.get("ModelMin") or {}, block.get("ModelMax") or {}
            lo = [lo[i] + f(m_lo.get(a)) for i, a in enumerate("XYZ")]
            hi = [hi[i] + f(m_hi.get(a)) for i, a in enumerate("XYZ")]
            out["sized"] = False
        out.update(min=[round(v, 3) for v in lo], max=[round(v, 3) for v in hi],
                   margin=xz_max(block.get("MarginMin") or 0, block.get("MarginMax") or 0) if forced else
                   xz_max(block.get(auto + "Min") or 0, block.get(auto + "Max") or 0))
    return out


# A swinging door's AI (Door.root, FieldDoor.event, PrivateDoorForNpc_Single in AI.Global) closes it through
# _LocalModule_e4e2d47532fa: SelectorDoorCanClose (DoorRadius 2.0, CloseWaitFrame 135, the same in all three). It's held
# open while any actor it perceives is within DoorRadius and on its -X side (main+0x5E58A0, FINDINGS 9.38).
DOOR_AIS = ("Door.root.ain", "PrivateDoorForNpc_Single.root.ain", "FieldDoor.event.root.ain")
DOOR_RADIUS, DOOR_WAIT_FRAMES = 2.0, 135


def is_door(loader, name):
    """Whether its AI (or its event AI) is a swinging door's."""
    actor = loader.load(f"Actor/{name}.engine__actor__ActorParam.bgyml") or {}
    components = actor.get("Components") or {}
    refs = []
    info = loader.load(components["AIInfoRef"]) if components.get("AIInfoRef") else None
    refs.append((info or {}).get("RootAIRef", ""))
    performer = loader.load(components["EventPerformerRef"]) if components.get("EventPerformerRef") else None
    refs.append((performer or {}).get("EventUniqueAin", ""))
    return any(ref.endswith(ai) for ref in refs for ai in DOOR_AIS)


def actor_clients(loader, name):
    actor = loader.load(f"Actor/{name}.engine__actor__ActorParam.bgyml")
    sender_ref = ((actor or {}).get("Components") or {}).get("AttentionSenderRef")
    sender = loader.load(sender_ref) if sender_ref else None
    clients = []
    for ref in (sender or {}).get("ClientList") or []:
        client = loader.load(ref)
        if not client:
            continue
        action = NAME_OF.get(client.get("ActionType"))
        if action in (None, "None", "Awake"):
            continue
        lock_on = NAME_OF.get(client.get("AttentionType")) == "LockOn" or bool(client.get("IsLockOnOnly"))
        areas = [a for a in (area(check, lock_on) for check in client.get("CheckList") or []) if a]
        if not areas:
            continue
        clients.append({"action": action, "lock_on": lock_on, "areas": areas})
    return clients


def main():
    nup = gamefiles.nup()
    resident = gamefiles.sarc(gamefiles.read("romfs/Pack/ResidentCommon.pack.zs"))
    names = sorted(p[len(PACK_PREFIX):-len(PACK_SUFFIX)] for p in nup.index
                   if p.startswith(PACK_PREFIX) and p.endswith(PACK_SUFFIX))
    actors, doors = {}, {}
    for name in names:
        try:
            files = gamefiles.sarc(gamefiles.read(PACK_PREFIX + name + PACK_SUFFIX))
            clients = actor_clients(Loader(files, resident), name)
            door = clients and is_door(Loader(files, resident), name)
        except Exception as e:  # a pack this reader can't handle: report and go on
            print(f"skip {name}: {e}")
            continue
        if clients:
            actors[name] = clients
            if door:
                doors[name] = {"radius": DOOR_RADIUS, "wait": DOOR_WAIT_FRAMES}
    with open(OUT, "w") as out:
        json.dump({"source": "game files (tools/interactions.py)", "actors": actors, "doors": doors}, out,
                  separators=(",", ":"))
    kinds = {}
    for clients in actors.values():
        for client in clients:
            kinds[client["action"]] = kinds.get(client["action"], 0) + 1
    print(f"{len(actors)} actors with interactions, {len(doors)} doors -> {OUT}")
    print(dict(sorted(kinds.items(), key=lambda kv: -kv[1])))


if __name__ == "__main__":
    main()
