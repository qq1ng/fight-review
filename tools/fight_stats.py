"""Per-player stats for one fight, computed from the raw log. Standard library only.

Definitions follow Elite Insights (EI) as TopStats uses them; tools/compare_topstats.py checks them against a
TopStats report.

Usage:
    python tools/fight_stats.py <file.zevtc>
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import (Log, SC, TIME, SRC, DST, VALUE, BUFF_DMG, SKILL, SRC_INST, SRC_MASTER, IFF, BUFF, RESULT,
                  SHIELDS, OFFCYCLE, STATECHANGE)

BOONS = {740: "Might", 725: "Fury", 1187: "Quickness", 30328: "Alacrity", 717: "Protection",
         718: "Regeneration", 726: "Vigor", 743: "Aegis", 1122: "Stability", 719: "Swiftness",
         26980: "Resistance", 873: "Resolution"}
CONDITIONS = {736: "Bleeding", 737: "Burning", 861: "Confusion", 723: "Poisoned", 19426: "Torment",
              720: "Blinded", 722: "Chilled", 721: "Crippled", 791: "Fear", 727: "Immobile", 26766: "Slow",
              27705: "Taunt", 742: "Weakness", 738: "Vulnerability"}

FOE = 1  # iff
BLOCK, EVADE, ABSORB = 3, 4, 6  # cbtresult

# Healing Stats flags in is_offcycle. Bit 0: target downed (as for strikes). Bit 7: reported by the healer's
# own Healing Stats. Heals with only bit 6 are what the recorder saw of someone else's healing: partial, and
# the only kind a healer without the addon ever has. TopStats counts bit-7 heals only and shows no healing at
# all for players without them; so do we (see heal_known).
HEAL_DOWNED, HEAL_SELF_REPORTED = 1, 128


class FightStats:
    """Per-player counters in self.p[addr]: heal, heal_downed, barrier, cleanses, cleanses_self, strips,
    evades, blocks, invulns. Plus healing, heal events and barrier per skill (casts are in tools/skills.py).

    TopStats' "Healing by Skill" table shows barrier under the skill's name and, for a skill that both heals
    and gives barrier (Troubadour's Crescendo), drops the healing. We keep the two apart."""

    def __init__(self, log):
        self.log = log
        agents = log.agents
        self.friends = {a for a, ag in agents.items() if ag["player"] and ag["account"]}
        self.foes = {a for a, ag in agents.items() if ag["player"] and not ag["account"]}

        # Minions (pets, spirits, clones) report their master's instance id; credit their heals to the master.
        inst_to_player = {}
        for e in log.events:
            if e[STATECHANGE] == 0 and e[SRC] in agents and agents[e[SRC]]["player"]:
                inst_to_player.setdefault(e[SRC_INST], e[SRC])
        self._inst_to_player = inst_to_player

        self.p = collections.defaultdict(collections.Counter)  # addr -> stat -> value
        self.heal_known = set()  # players whose own Healing Stats reported their heals
        self.heal_by_skill = collections.defaultdict(collections.Counter)  # addr -> skill id -> healing
        self.heal_hits_by_skill = collections.defaultdict(collections.Counter)  # addr -> skill id -> heal events
        self.barrier_by_skill = collections.defaultdict(collections.Counter)  # addr -> skill id -> barrier
        self.strip_times = collections.defaultdict(list)  # addr -> [time]
        self.cleanse_times = collections.defaultdict(list)  # addr -> [time], cleanses on others
        self._run()

    def owner(self, addr, master_inst):
        """The player behind an agent: itself, or its master if it's a minion."""
        if addr in self.log.agents and self.log.agents[addr]["player"]:
            return addr
        return self._inst_to_player.get(master_inst) if master_inst else None

    def _run(self):
        ext = SC["EXTENSIONCOMBAT"]
        rm_all = SC["BUFFREMOVE_ALL"]
        for e in self.log.events:
            sc = e[STATECHANGE]
            if sc == ext:
                self._heal(e)
            elif sc == rm_all:
                self._removal(e)
            elif sc == 0:
                self._strike(e)

    def _heal(self, e):
        # Direct heals carry -value, heal ticks -buff_dmg; is_shields marks barrier.
        if not e[OFFCYCLE] & HEAL_SELF_REPORTED:
            return
        amount = -e[VALUE] if e[VALUE] < 0 else -e[BUFF_DMG] if e[BUFF_DMG] < 0 else 0
        healer = self.owner(e[SRC], e[SRC_MASTER])
        if not amount or healer not in self.friends or e[DST] not in self.friends:
            return
        self.heal_known.add(healer)
        s = self.p[healer]
        if e[SHIELDS]:
            s["barrier"] += amount
            self.barrier_by_skill[healer][e[SKILL]] += amount
        elif e[OFFCYCLE] & HEAL_DOWNED:
            s["heal_downed"] += amount
        else:
            s["heal"] += amount
            self.heal_by_skill[healer][e[SKILL]] += amount
            self.heal_hits_by_skill[healer][e[SKILL]] += 1

    def _removal(self, e):
        # src had the buff removed, dst removed it. One event per buff removed, whatever the stack count
        # (in result). Only removals by the player itself count, not by their pets or minions: that's what
        # TopStats shows (a Druid's pet cleanses are left out).
        remover, target, sid = e[DST], e[SRC], e[SKILL]
        if remover not in self.friends:
            return
        if sid in BOONS and e[IFF] == FOE:  # any foe, including pets, clones and siege
            self.p[remover]["strips"] += 1
            self.strip_times[remover].append(e[TIME])
        elif sid in CONDITIONS and target in self.friends:
            self.p[remover]["cleanses_self" if target == remover else "cleanses"] += 1
            if target != remover:
                self.cleanse_times[remover].append(e[TIME])

    def _strike(self, e):
        # Incoming hits the player avoided. Invulnerability also absorbs condition ticks, and those count.
        if e[DST] not in self.friends:
            return
        r = e[RESULT]
        if r == ABSORB:
            self.p[e[DST]]["invulns"] += 1
        elif e[BUFF]:
            return
        elif r == EVADE:
            self.p[e[DST]]["evades"] += 1
        elif r == BLOCK:
            self.p[e[DST]]["blocks"] += 1


def main(path):
    log = Log(path)
    fs = FightStats(log)
    heal_cols = ["heal", "heal_downed", "barrier"]
    cols = heal_cols + ["cleanses", "cleanses_self", "strips", "evades", "blocks", "invulns"]
    print(f"{'player':28}" + "".join(f"{c:>14}" for c in cols))
    for addr in sorted(fs.friends, key=lambda a: -fs.p[a]["heal"]):
        s = fs.p[addr]
        unknown = lambda c: c in heal_cols and addr not in fs.heal_known
        print(f"{log.agents[addr]['name'][:27]:28}" + "".join(f"{'-' if unknown(c) else s[c]:>14}" for c in cols))


if __name__ == "__main__":
    main(sys.argv[1])
