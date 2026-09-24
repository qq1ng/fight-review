"""Reader for arcdps .zevtc / .evtc combat logs (revision 1). Standard library only.

Layout per reference/arcdps_evtc_README.txt and reference/writeencounter.cpp:

    header   16 bytes   "EVTC" + arcdps build "yyyymmdd" + revision (u8) + target species (u16, 1 = WvW) + pad
    u32      agent count, then 96-byte agents
    u32      skill count, then 68-byte skills (i32 id + 64-byte utf8 name)
    events   64-byte cbtevent records until end of file

Usage:
    python tools/evtc.py <file.zevtc>        prints a short summary of one log
"""
import collections
import os
import re
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

# cbtevent, revision 1. Offsets are easy to get wrong by hand (it happened twice): use these names.
EVENT = struct.Struct("<QQQiiIIHHHHBBBBBBBBBBBBI")
(TIME, SRC, DST, VALUE, BUFF_DMG, OVERSTACK, SKILL, SRC_INST, DST_INST, SRC_MASTER, DST_MASTER,
 IFF, BUFF, RESULT, ACTIVATION, BUFF_REMOVE, NINETY, FIFTY, MOVING, STATECHANGE, FLANKING, SHIELDS,
 OFFCYCLE, PAD61) = range(24)  # PAD61: bytes 60-63 as u32, the buff stack ("trackable") id for buff events

# evtc_agent: addr u64, prof u32, is_elite u32, six i16 (toughness, concentration, healing, hitbox width,
# condition, hitbox height), name[64] (+4 pad). Players' name is "character\0:account\0subgroup\0".
AGENT = struct.Struct("<QII6h64s4x")
NOT_PLAYER = 0xFFFFFFFF

PROFESSIONS = {1: "Guardian", 2: "Warrior", 3: "Engineer", 4: "Ranger", 5: "Thief", 6: "Elementalist",
               7: "Mesmer", 8: "Necromancer", 9: "Revenant"}
ELITE_SPECS = {27: "Dragonhunter", 62: "Firebrand", 65: "Willbender", 81: "Luminary",
               18: "Berserker", 61: "Spellbreaker", 68: "Bladesworn", 74: "Paragon",
               43: "Scrapper", 57: "Holosmith", 70: "Mechanist", 75: "Amalgam",
               5: "Druid", 55: "Soulbeast", 72: "Untamed", 78: "Galeshot",
               7: "Daredevil", 58: "Deadeye", 71: "Specter", 77: "Antiquary",
               48: "Tempest", 56: "Weaver", 67: "Catalyst", 80: "Evoker",
               40: "Chronomancer", 59: "Mirage", 66: "Virtuoso", 73: "Troubadour",
               34: "Reaper", 60: "Scourge", 64: "Harbinger", 76: "Ritualist",
               52: "Herald", 63: "Renegade", 69: "Vindicator", 79: "Conduit"}


def spec_name(agent):
    """Elite spec of a player agent as recorded in this log, else the core profession."""
    return ELITE_SPECS.get(agent["elite"]) or PROFESSIONS.get(agent["prof"], "?")


def _statechange_names():
    """The CBTS_ enum in the order the readme lists it; its position is the value."""
    names = []
    readme = os.path.join(HERE, "..", "reference", "arcdps_evtc_README.txt")
    with open(readme, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*CBTS_([A-Z0-9_]+)\s*(=\s*0\s*)?,?\s*//", line)
            if m:
                names.append(m.group(1))
    return names


SC_NAMES = _statechange_names()
SC = {name: i for i, name in enumerate(SC_NAMES)}  # SC["ANIMATIONSTART"] == 67


class Log:
    def __init__(self, path):
        if path.endswith(".zevtc"):
            with zipfile.ZipFile(path) as z:
                data = z.read(z.namelist()[0])
        else:
            with open(path, "rb") as f:
                data = f.read()
        if data[:4] != b"EVTC":
            raise ValueError(f"{path}: not an evtc file")
        self.path = path
        self.arc_build = data[4:12].decode()
        self.revision = data[12]
        self.species = struct.unpack_from("<H", data, 13)[0]  # 1 = WvW, 2 = map
        if self.revision != 1:
            raise ValueError(f"{path}: unsupported revision {self.revision}")

        pos = 16
        (count,) = struct.unpack_from("<I", data, pos)
        pos += 4
        self.agents = {}
        for i in range(count):
            addr, prof, elite, *_, raw = AGENT.unpack_from(data, pos + i * AGENT.size)
            parts = raw.split(b"\0")
            text = lambda n: parts[n].decode("utf-8", "replace") if len(parts) > n else ""
            player = elite != NOT_PLAYER
            self.agents[addr] = {
                "addr": addr, "prof": prof, "elite": elite, "player": player,
                "name": text(0),
                "account": text(1).lstrip(":") if player else "",  # stored as ":Name.1234"
                # Squad members carry their subgroup; enemies in WvW are anonymised rank names with no account.
                "subgroup": int(text(2)) if player and text(2).isdigit() else 0,
            }
        pos += count * AGENT.size

        (count,) = struct.unpack_from("<I", data, pos)
        pos += 4
        self.skills = {}
        for i in range(count):
            sid, raw = struct.unpack_from("<i64s", data, pos + i * 68)
            self.skills[sid] = raw.split(b"\0")[0].decode("utf-8", "replace")
        pos += count * 68

        end = pos + (len(data) - pos) // EVENT.size * EVENT.size
        self.events = list(EVENT.iter_unpack(data[pos:end]))

    def squad(self):
        return [a for a in self.agents.values() if a["player"] and a["subgroup"] > 0]

    def present_squad(self):
        """Squad members who took part: entered combat, or dealt or took a hit. The agent table also lists
        squad members who were elsewhere (a benched player standing in spawn); TopStats leaves those out."""
        squad = {a["addr"] for a in self.squad()}
        present = set()
        enter = SC["ENTERCOMBAT"]
        for e in self.events:
            sc = e[STATECHANGE]
            if sc == enter and e[SRC] in squad:
                present.add(e[SRC])
            elif sc == 0:
                if e[SRC] in squad:
                    present.add(e[SRC])
                if e[DST] in squad:
                    present.add(e[DST])
        return [self.agents[a] for a in present]

    def enemies(self):
        return [a for a in self.agents.values() if a["player"] and not a["account"]]

    def duration_ms(self):
        # Some statechanges use the time field for other data (see the readme), so only combat events count.
        times = [e[TIME] for e in self.events if e[STATECHANGE] == 0 and e[TIME]]
        return max(times) - min(times) if times else 0


def summary(path):
    log = Log(path)
    counts = collections.Counter(e[STATECHANGE] for e in log.events)
    squad = {a["addr"] for a in log.squad()}
    enemies = {a["addr"] for a in log.enemies()}
    side = lambda addr: "squad" if addr in squad else "enemy" if addr in enemies else None
    life = collections.Counter()
    for e in log.events:
        if e[STATECHANGE] in (SC["CHANGEDOWN"], SC["CHANGEDEAD"]) and side(e[SRC]):
            life[(side(e[SRC]), "downs" if e[STATECHANGE] == SC["CHANGEDOWN"] else "deaths")] += 1

    print(f"{os.path.basename(path)}  arcdps {log.arc_build}  species {log.species}  "
          f"{log.duration_ms() / 1000:.0f} s  {len(log.events)} events")
    print(f"squad {len(squad)}  enemy players {len(enemies)}")
    print(f"squad downs {life[('squad', 'downs')]}  deaths {life[('squad', 'deaths')]}  |  "
          f"enemy downs {life[('enemy', 'downs')]}  deaths {life[('enemy', 'deaths')]}")
    for value, n in counts.most_common(14):
        name = SC_NAMES[value] if value < len(SC_NAMES) else "?"
        print(f"  {value:3} {name:22} {n}")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        summary(arg)
