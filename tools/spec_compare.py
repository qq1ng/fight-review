"""You against the best other player on your spec, skill by skill, over a set of fights. Standard library only.

This is how the user reads TopStats: find who on the same spec did best, then look at which of their skills
produced the most, whether they cast them more often or got more out of each cast, and when they cast them.

What "best" means depends on the role (--by):
    heal        healing + barrier (needs Healing Stats data from the player)
    damage      damage to enemy players (TopStats "dmg", what its high scores use); minions count for
                their master
    damage-all  damage to anything hostile: players, pets, siege, NPCs (TopStats "dmgAll")
    strips      boons removed from enemies
    cleanses    conditions removed from allies
    <boon>      a boon given to other squad members, in seconds (stack-seconds for stability and might);
                --group counts only the own subgroup

Rates are per minute of active time (fight time not dead), summed over the fights where the player played
the spec. "Them" is one player: the best other one, or --vs ACCOUNT.

Boons, strips and cleanses don't say which skill caused them; each is credited to the cast that was in
progress (tools/skills.py SkillUsage.attribute). "no cast" collects what no cast explains: traits, relics,
sigils. Healing and damage carry their skill in the log.

Timing: share of a skill's casts that went into one of our damage spikes (within 2 s), ahead of an enemy
spike (up to 4 s before) or answering one (up to 3 s after); spikes as in tools/timeline.py. The first row
is the share of fight time in each window: what casting at random would score.

Usage:
    python tools/spec_compare.py <account> <log or folder> [more ...] [--from 20260915-2020] [--to 20260915-2100]
                                 [--spec Troubadour] [--by heal|damage|damage-all|strips|cleanses|stability|...] [--group]
                                 [--vs ACCOUNT]
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import Log, spec_name
from skills import SkillUsage
from boons import BoonGeneration
from fight_stats import BOONS
from timeline import Timeline, classify, baseline

NO_CAST = 0  # attribution key for what no cast explains
BOON_IDS = {name.lower(): bid for bid, name in BOONS.items()}
UNITS = {"heal": "heal+barrier", "damage": "damage to players", "damage-all": "damage", "strips": "strips",
         "cleanses": "cleanses"}
TIMING = ["into ours", "ahead of theirs", "answering theirs"]


class Player:
    def __init__(self, account, name):
        self.account, self.name = account, name
        self.fights = 0
        self.active_ms = 0
        self.heal_known = False  # Healing Stats data in any of the fights: the addon is per player
        self.casts = collections.Counter()
        self.value = collections.Counter()  # skill -> metric
        self.timing = collections.defaultdict(collections.Counter)  # skill -> timing class -> casts
        self.base = collections.Counter()  # timing class -> active seconds in it; "all" -> active seconds

    def per_min(self, v):
        return v * 60000 / self.active_ms if self.active_ms else 0.0

    def total(self):
        return sum(self.value.values())


def log_paths(args, start, end):
    paths = []
    for a in args:
        if os.path.isdir(a):
            paths += [os.path.join(a, n) for n in sorted(os.listdir(a)) if n.endswith((".zevtc", ".evtc"))]
        else:
            paths.append(a)
    stamp = lambda p: os.path.basename(p)[:13]
    return [p for p in paths if (not start or stamp(p) >= start) and (not end or stamp(p) <= end)]


def collect(paths, spec, metric, group_only):
    players, names = {}, {NO_CAST: "no cast (traits, relics, sigils)"}
    boon = BOON_IDS.get(metric)
    for path in paths:
        log = Log(path)
        su = SkillUsage(log)
        tl = Timeline(su)
        bg = BoonGeneration(log) if boon else None
        for a in su.players:
            ag = log.agents[a]
            if spec_name(ag) != spec:
                continue
            p = players.setdefault(ag["account"], Player(ag["account"], ag["name"]))
            p.fights += 1
            p.active_ms += su.active_ms[a]
            p.heal_known |= a in su.fs.heal_known
            for s, c in su.casts[a].items():
                p.casts[s] += c
                names[s] = su.name(s)
            for s, times in su.cast_times[a].items():
                for t in times:
                    for cls in classify(t, tl):
                        p.timing[s][cls] += 1
            counts, seconds = baseline(tl, su.end - su.start)
            p.base.update(counts)
            p.base["all"] += seconds

            if metric == "heal":
                sources = [su.heal[a], su.fs.barrier_by_skill[a]]
            elif metric == "damage":
                sources = [su.damage_players[a]]
            elif metric == "damage-all":
                sources = [su.damage[a]]
            else:
                sources = []
            for source in sources:
                for s, v in source.items():
                    p.value[s] += v
                    names[s] = su.name(s)
            if metric in ("strips", "cleanses"):
                times = su.fs.strip_times[a] if metric == "strips" else su.fs.cleanse_times[a]
                for t in times:
                    p.value[su.attribute(a, t) or NO_CAST] += 1
            if boon:
                group = log.agents[a]["subgroup"]
                for src, tgt, b, applied, ms in bg.by_stack:
                    if src != a or b != boon or tgt == a or tgt not in bg.squad:
                        continue
                    if group_only and log.agents[tgt]["subgroup"] != group:
                        continue
                    p.value[su.attribute(a, applied) or NO_CAST] += ms / 1000
    return players, names


def main(args):
    opt = lambda k, d=None: args[args.index(k) + 1] if k in args else d
    flags = {"--from", "--to", "--spec", "--by", "--vs"}
    pos = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in flags)]
    me, sources = pos[0], pos[1:]
    metric = opt("--by", "heal").lower()
    group_only = "--group" in args
    if metric not in UNITS and metric not in BOON_IDS:
        sys.exit(f"--by must be one of {', '.join(list(UNITS) + sorted(BOON_IDS))}")
    paths = log_paths(sources, opt("--from"), opt("--to"))

    spec = opt("--spec")
    if not spec:  # the spec this account played most in these fights
        seen = collections.Counter()
        for path in paths:
            for ag in Log(path).agents.values():
                if ag["account"] == me:
                    seen[spec_name(ag)] += 1
        if not seen:
            sys.exit(f"{me} is in none of the {len(paths)} logs")
        spec = seen.most_common(1)[0][0]

    players, names = collect(paths, spec, metric, group_only)
    if me not in players:
        sys.exit(f"{me} never played {spec} in these logs")
    known = lambda p: metric != "heal" or p.heal_known
    ranked = sorted(players.values(), key=lambda p: (not known(p), -p.per_min(p.total())))
    you = players[me]
    other = players.get(opt("--vs")) if opt("--vs") else next(
        (p for p in ranked if p.account != me and known(p)), None)

    unit = UNITS.get(metric) or f"{BOONS[BOON_IDS[metric]].lower()} s given to " + ("subgroup" if group_only else "squad")
    print(f"{spec}: {len(players)} players over {len(paths)} logs, ranked by {unit} per minute\n")
    # In GvG everyone shows as a WvW rank name; add the account where names repeat
    repeated = {n for n, c in collections.Counter(p.name for p in ranked).items() if c > 1}
    label = lambda p: f"{p.name} ({p.account})" if p.name in repeated else p.name
    for i, p in enumerate(ranked, 1):
        mark = "  <- you" if p.account == me else "  <- compared" if other and p.account == other.account else ""
        value = f"{p.per_min(p.total()):>10,.1f}" if known(p) else f"{'unknown':>10}"
        print(f"  {i:2}. {label(p)[:34]:34} {value}  {p.fights:>2} fights, {p.active_ms / 60000:5.1f} min{mark}")
    if not other:
        return
    if not known(you):
        print("\n(no Healing Stats data from you in these fights: your healing is unknown)")

    short = {"heal": "heal", "damage": "dmg", "damage-all": "dmg", "strips": "strips",
             "cleanses": "cleanse"}.get(metric, "boon s")
    print(f"\nYou ({label(you)}) against {label(other)}, per minute of active time")
    skills = set(you.value) | set(other.value) | set(you.casts) | set(other.casts)
    skills = sorted(skills, key=lambda s: -(you.per_min(you.value[s]) + other.per_min(other.value[s]))
                    - 1e-9 * (you.casts[s] + other.casts[s]))
    print(f"{'skill':32}{'casts/min':>17}{short + '/min':>22}{short + '/cast':>22}")
    print(f"{'':32}{'you':>8}{'them':>9}{'you':>11}{'them':>11}{'you':>11}{'them':>11}")
    for s in skills[:22]:
        cells = []
        for p in (you, other):
            cells.append(p.per_min(p.casts[s]))
        vals = [p.per_min(p.value[s]) for p in (you, other)]
        per_cast = [p.value[s] / p.casts[s] if p.casts[s] and s != NO_CAST else None for p in (you, other)]
        fmt = lambda v: f"{v:>11,.1f}" if v is not None and v < 100 else f"{v:>11,.0f}" if v is not None else f"{'-':>11}"
        print(f"{names.get(s, str(s))[:31]:32}{cells[0]:>8.2f}{cells[1]:>9.2f}"
              f"{fmt(vals[0])}{fmt(vals[1])}{fmt(per_cast[0])}{fmt(per_cast[1])}")

    print(f"\nTiming: share of casts into our spikes / ahead of theirs / answering theirs")
    print(f"{'skill':32}{'casts':>13}{'into ours':>16}{'ahead':>16}{'answering':>16}")
    print(f"{'':32}{'you':>6}{'them':>7}" + f"{'you':>8}{'them':>8}" * 3)
    base = lambda p, cls: f"{100 * p.base[cls] / max(p.base['all'], 1):>7.0f}%"
    print(f"{'(share of fight time)':32}{'':13}" + "".join(base(you, c) + base(other, c) for c in TIMING))
    by_casts = sorted((s for s in set(you.casts) | set(other.casts) if s != NO_CAST),
                      key=lambda s: -(you.casts[s] + other.casts[s]))
    for s in by_casts[:16]:
        share = lambda p, cls: f"{100 * p.timing[s][cls] / p.casts[s]:>7.0f}%" if p.casts[s] else f"{'-':>8}"
        print(f"{names.get(s, str(s))[:31]:32}{you.casts[s]:>6}{other.casts[s]:>7}"
              + "".join(share(you, c) + share(other, c) for c in TIMING))


if __name__ == "__main__":
    main(sys.argv[1:])
