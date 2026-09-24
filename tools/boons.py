"""Boon generation per player from the raw log, following each stack by its id. Standard library only.

The log tells us everything we need without simulating durations:

    BUFFAPPLY / BUFFINITIAL   a stack appears on dst, applied by src; pad61 is its stack id,
                              is_shields says whether it is active (ticking) from the start
    BUFFACTIVE                stack dst_agent of src_agent starts ticking
    BUFFDEACTIVE              stack pad61 of src_agent stops ticking (queued behind another)
    BUFFREMOVE_SINGLE         stack pad61 of src_agent is gone (expired, stripped, cleansed, replaced)
    BUFFINFO                  per buff: stacking type (pad61) and stack limit (src_master_instid)

A queued boon (quickness, protection, ...) only covers its target while one of its stacks ticks, so a
source is credited with the time its stacks were ticking. An intensity boon (stability, might) counts every
stack present, so a source is credited with the time its stacks were present.

Elite Insights' "squad generation", which TopStats reports per fight, is then per source S:
    sum over squad members T != S of credited time on T / (fight duration * (squad size - 1))
as average stacks for intensity boons and as % uptime for queued boons.

Usage:
    python tools/boons.py <file.zevtc>
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import Log, SC, TIME, SRC, DST, VALUE, SKILL, SRC_MASTER, STATECHANGE, SHIELDS, PAD61
from fight_stats import BOONS

# BUFFINFO stacking types that count every stack (intensity). The others queue and count one ticking stack.
INTENSITY_TYPES = {0, 4}


class BoonGeneration:
    """self.credit[(source, target, boon)] = ms credited; self.window = (start, end) in log time;
    self.by_stack = [(source, target, boon, applied at, ms credited)] for attributing boons to skills."""

    def __init__(self, log):
        self.log = log
        self.squad = {a["addr"] for a in log.present_squad()}
        self.window = self._window()
        self.intensity = self._stacking()
        self.credit = collections.Counter()
        self.by_stack = []
        self._run()

    def _window(self):
        # Squad combat start/end: gives the fight duration TopStats shows. Fall back to strike events.
        start = [e[TIME] for e in self.log.events if e[STATECHANGE] == SC["SQCOMBATSTART"]]
        end = [e[TIME] for e in self.log.events if e[STATECHANGE] == SC["SQCOMBATEND"]]
        if start and end:
            return min(start), max(end)
        times = [e[TIME] for e in self.log.events if e[STATECHANGE] == 0]
        return min(times), max(times)

    def _stacking(self):
        intensity = {}
        for e in self.log.events:
            if e[STATECHANGE] == SC["BUFFINFO"] and e[SKILL] in BOONS:
                intensity[e[SKILL]] = (e[PAD61] & 0xFF) in INTENSITY_TYPES
        return intensity

    def _run(self):
        apply_ = {SC["BUFFAPPLY"], SC["BUFFINITIAL"]}
        active, deactive = SC["BUFFACTIVE"], SC["BUFFDEACTIVE"]
        remove = SC["BUFFREMOVE_SINGLE"]
        # (target, stack id) -> [source, boon, counting since (None if not counting), applied at]
        stacks = {}

        def stop(key, t):
            st = stacks.get(key)
            if st and st[2] is not None:
                ms = self._add(st[0], key[0], st[1], st[2], t)
                if ms:
                    self.by_stack.append((st[0], key[0], st[1], st[3], ms))
                st[2] = None

        for e in self.log.events:
            sc = e[STATECHANGE]
            if sc in apply_:
                if e[SKILL] not in BOONS or e[DST] not in self.squad:
                    continue
                boon = e[SKILL]
                counting = self.intensity.get(boon, False) or e[SHIELDS]
                stacks[(e[DST], e[PAD61])] = [e[SRC], boon, e[TIME] if counting else None, e[TIME]]
            elif sc == active:
                st = stacks.get((e[SRC], e[DST]))
                if st and st[2] is None:
                    st[2] = e[TIME]
            elif sc == deactive:
                st = stacks.get((e[SRC], e[PAD61]))
                if st and not self.intensity.get(st[1], False):
                    stop((e[SRC], e[PAD61]), e[TIME])
            elif sc == remove:
                key = (e[SRC], e[PAD61])
                if key in stacks:
                    stop(key, e[TIME])
                    del stacks[key]
        for key in list(stacks):
            stop(key, self.window[1])

    def _add(self, source, target, boon, t0, t1):
        t0, t1 = max(t0, self.window[0]), min(t1, self.window[1])
        if t1 > t0:
            self.credit[(source, target, boon)] += t1 - t0
            return t1 - t0
        return 0

    def squad_generation(self, source, boon):
        """EI squad generation: average stacks (intensity) or % uptime (queued) given to the rest of the squad."""
        others = self.squad - {source}
        if not others:
            return 0.0
        total = sum(self.credit[(source, t, boon)] for t in others)
        value = total / ((self.window[1] - self.window[0]) * len(others))
        return value if self.intensity.get(boon, False) else 100 * value


    def uptime(self, targets, boon):
        """Average over targets: stacks (intensity) or % uptime (queued). A queued boon has one ticking stack
        at a time, so summing every source's credit gives the uptime."""
        targets = list(targets)
        if not targets:
            return 0.0
        total = sum(ms for (s, t, b), ms in self.credit.items() if b == boon and t in targets)
        value = total / ((self.window[1] - self.window[0]) * len(targets))
        return value if self.intensity.get(boon, False) else 100 * value

    def suppliers(self, targets, boon):
        """Sources of a boon on these targets, largest share first: [(source, share 0..1)]."""
        targets = set(targets)
        by_source = collections.Counter()
        for (s, t, b), ms in self.credit.items():
            if b == boon and t in targets:
                by_source[s] += ms
        total = sum(by_source.values())
        return [(s, ms / total) for s, ms in by_source.most_common()] if total else []


def main(path):
    log = Log(path)
    bg = BoonGeneration(log)
    shown = [1122, 1187, 740, 717, 743, 26980, 873, 725, 30328]
    name = lambda a: log.agents[a]["name"] if a in log.agents else "?"
    print(f"window {(bg.window[1] - bg.window[0]) / 1000:.1f} s, squad {len(bg.squad)}\n")
    print("Squad generation (TopStats' per-fight numbers): stacks for stability and might, % for the rest")
    print(f"{'player':28}" + "".join(f"{BOONS[b][:10]:>11}" for b in shown))
    for addr in sorted(bg.squad, key=lambda a: -bg.squad_generation(a, 1122)):
        print(f"{name(addr)[:27]:28}" + "".join(f"{bg.squad_generation(addr, b):>11.3f}" for b in shown))

    groups = collections.defaultdict(list)
    for a in bg.squad:
        groups[log.agents[a]["subgroup"]].append(a)
    print("\nUptime by subgroup: stacks for stability and might, % for the rest")
    print(f"{'subgroup':10}" + "".join(f"{BOONS[b][:10]:>11}" for b in shown))
    for g in sorted(groups):
        print(f"{g:<3}({len(groups[g])})   " + "".join(f"{bg.uptime(groups[g], b):>11.2f}" for b in shown))
    print("\nWho gave each subgroup its stability")
    for g in sorted(groups):
        top = bg.suppliers(groups[g], 1122)[:4]
        print(f"  {g}: " + ", ".join(f"{name(s)[:18]} {share:.0%}" for s, share in top))


if __name__ == "__main__":
    main(sys.argv[1])
