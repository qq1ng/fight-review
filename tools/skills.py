"""Skill usage per player from the raw log: casts, cast times, healing and damage per skill. Standard library only.

Casts are ANIMATIONSTART events, plus instant skills (no animation, e.g. Power Return) found by Elite
Insights' rules in tools/instant_casts.py.

Usage:
    python tools/skills.py <file.zevtc> [account]
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import Log, SC, TIME, SRC, DST, VALUE, BUFF_DMG, SKILL, SRC_MASTER, BUFF, RESULT, STATECHANGE
from fight_stats import FightStats
from instant_casts import instant_casts, NAMES as EI_NAMES, FALLBACK_NAMES

# Skill ids the log's skill table leaves unnamed; names as Elite Insights shows them.
SPECIAL = {23275: "Dodge", 23285: "Weapon Stow", 1066: "Resurrect", -2: "Weapon Swap"}
WEAPON_SWAP = -2

# Attribution windows: an animated cast covers its animation plus this much, an instant cast this much either side
CAST_SLACK = 250
INSTANT_SLACK = 100

# cbtresult values that are damage (strikes and condition ticks), not avoided hits or signals
DAMAGE_RESULTS = {0, 1, 2, 5, 8, 9, 10, 13, 14, 15, 16, 17, 18}


class SkillUsage:
    """Per player (addr): casts[skill], cast_times[skill] (ms from fight start), heal[skill], heal_hits[skill],
    damage[skill] (on anything hostile), damage_players[skill] (on enemy players), and active_ms (time in the
    fight not dead)."""

    def __init__(self, log, fight_stats=None):
        self.log = log
        self.fs = fight_stats or FightStats(log)
        self.players = {a["addr"] for a in log.present_squad()}
        start = [e[TIME] for e in log.events if e[STATECHANGE] == SC["SQCOMBATSTART"]]
        end = [e[TIME] for e in log.events if e[STATECHANGE] == SC["SQCOMBATEND"]]
        strikes = [e[TIME] for e in log.events if e[STATECHANGE] == 0]
        self.start = min(start) if start else min(strikes)
        self.end = max(end) if end else max(strikes)

        self.casts = collections.defaultdict(collections.Counter)
        self.cast_times = collections.defaultdict(lambda: collections.defaultdict(list))
        self.windows = collections.defaultdict(list)  # addr -> [(start, end, skill)] in log time, for attribution
        self.damage = collections.defaultdict(collections.Counter)  # on anything hostile (TopStats "dmgAll")
        self.damage_players = collections.defaultdict(collections.Counter)  # on enemy players (TopStats "dmg")
        self.heal = self.fs.heal_by_skill
        self.heal_hits = self.fs.heal_hits_by_skill
        self.active_ms = {}
        self._run()

    def name(self, skill):
        return (SPECIAL.get(skill) or EI_NAMES.get(skill) or self.log.skills.get(skill)
                or FALLBACK_NAMES.get(skill) or str(skill))

    def _run(self):
        cast, swap = SC["ANIMATIONSTART"], SC["WEAPSWAP"]
        dead_since, dead_ms = {}, collections.Counter()
        down, dead, up, spawn = SC["CHANGEDOWN"], SC["CHANGEDEAD"], SC["CHANGEUP"], SC["SPAWN"]
        for e in self.log.events:
            sc, src = e[STATECHANGE], e[SRC]
            if sc == cast and src in self.players:
                self.casts[src][e[SKILL]] += 1
                self.cast_times[src][e[SKILL]].append(e[TIME] - self.start)
                # value: ms to the last trigger point; buff_dmg: ms until control returns
                self.windows[src].append((e[TIME], e[TIME] + max(e[VALUE], e[BUFF_DMG], 0) + CAST_SLACK, e[SKILL]))
            elif sc == swap and src in self.players:
                self.casts[src][WEAPON_SWAP] += 1
                self.cast_times[src][WEAPON_SWAP].append(e[TIME] - self.start)
            elif sc == 0 and e[RESULT] in DAMAGE_RESULTS:
                owner = self.fs.owner(src, e[SRC_MASTER])
                if owner in self.players and e[DST] not in self.fs.friends:
                    dmg = e[BUFF_DMG] if e[BUFF] else e[VALUE]
                    self.damage[owner][e[SKILL]] += dmg
                    if e[DST] in self.fs.foes:
                        self.damage_players[owner][e[SKILL]] += dmg
            elif sc == dead and src in self.players:
                dead_since.setdefault(src, e[TIME])
            elif sc in (up, down, spawn) and src in dead_since:
                dead_ms[src] += e[TIME] - dead_since.pop(src)
        for a, t in dead_since.items():
            dead_ms[a] += self.end - t
        for a in self.players:
            self.active_ms[a] = max(0, self.end - self.start - dead_ms[a])
        for a, skills in instant_casts(self.log).items():
            if a in self.players:
                for s, times in skills.items():
                    self.casts[a][s] += len(times)
                    self.cast_times[a][s] += [t - self.start for t in times]
                    self.windows[a] += [(t - INSTANT_SLACK, t + INSTANT_SLACK, s) for t in times]
        for w in self.windows.values():
            w.sort()

    def attribute(self, addr, t):
        """The skill whose cast was in progress at time t (log time): the latest cast whose window holds t,
        or None (a trait, relic or sigil proc, or something the log can't tie to a cast). An estimate: the
        log doesn't say which skill applied a boon or removed one."""
        # An instant cast next to t explains it better than a longer animation that happens to be running;
        # among instant casts the closest wins, among animations the latest.
        instant, animated = None, None
        for start, end, skill in self.windows.get(addr, ()):
            if start > t:
                break
            if t > end:
                continue
            if end - start == 2 * INSTANT_SLACK:
                d = abs(t - (start + INSTANT_SLACK))
                if instant is None or d < instant[0]:
                    instant = (d, skill)
            else:
                animated = skill
        return instant[1] if instant else animated
