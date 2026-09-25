"""A fight's damage over time and its spikes: when our squad's damage landed together, and when the enemy's did.

Damage per second, out (squad and its minions on anyone not friendly) and in (anyone not friendly on squad
members). A spike is a second that is the highest within 2 s either side, at least 1.4x the fight's median
second and at least 50% of its biggest second (tuned with tools/spike_sweep.py, 2026-09-24).

Usage:
    python tools/timeline.py <file.zevtc>
"""
import collections
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import Log, SRC, DST, VALUE, BUFF_DMG, SRC_MASTER, BUFF, RESULT, STATECHANGE, TIME

SPIKE_RADIUS_S = 2
SPIKE_OVER_MEDIAN = 1.4
SPIKE_OF_MAX = 0.5


def spikes(series):
    """Seconds (indices) that are spikes in a per-second damage series."""
    busy = [v for v in series if v > 0]
    if not busy:
        return []
    floor = max(SPIKE_OVER_MEDIAN * statistics.median(busy), SPIKE_OF_MAX * max(busy))
    out = []
    for i, v in enumerate(series):
        window = series[max(0, i - SPIKE_RADIUS_S):i + SPIKE_RADIUS_S + 1]
        if v >= floor and v == max(window) and (not out or i - out[-1] > SPIKE_RADIUS_S):
            out.append(i)
    return out


class Timeline:
    """out_per_s / in_per_s: damage per second from fight start; our_spikes / their_spikes: ms from start
    (the middle of the spike second)."""

    def __init__(self, skill_usage):
        su = skill_usage
        from skills import DAMAGE_RESULTS
        n = (su.end - su.start) // 1000 + 1
        self.out_per_s, self.in_per_s = [0] * n, [0] * n
        for e in su.log.events:
            if e[STATECHANGE] != 0 or e[RESULT] not in DAMAGE_RESULTS:
                continue
            dmg = e[BUFF_DMG] if e[BUFF] else e[VALUE]
            i = (e[TIME] - su.start) // 1000
            if dmg <= 0 or not 0 <= i < n:
                continue
            owner = su.fs.owner(e[SRC], e[SRC_MASTER])
            if owner in su.players and e[DST] not in su.fs.friends:
                self.out_per_s[i] += dmg
            elif e[DST] in su.players and owner not in su.fs.friends:
                self.in_per_s[i] += dmg
        self.our_spikes = [s * 1000 + 500 for s in spikes(self.out_per_s)]
        self.their_spikes = [s * 1000 + 500 for s in spikes(self.in_per_s)]


# Cast timing classes, ms relative to a spike's middle
INTO_OUR_SPIKE = 2000  # |cast - our spike| <= this
AHEAD_OF_THEIRS = 4000  # cast up to this long before their spike
ANSWERING_THEIRS = 3000  # cast up to this long after their spike


def classify(t, timeline):
    """Timing classes a cast at t (ms from fight start) falls into."""
    out = set()
    if any(abs(t - s) <= INTO_OUR_SPIKE for s in timeline.our_spikes):
        out.add("into ours")
    if any(0 < s - t <= AHEAD_OF_THEIRS for s in timeline.their_spikes):
        out.add("ahead of theirs")
    if any(0 <= t - s <= ANSWERING_THEIRS for s in timeline.their_spikes):
        out.add("answering theirs")
    return out


def baseline(timeline, fight_ms):
    """Share of the fight's seconds that fall in each timing class: what random casting would score."""
    counts = collections.Counter()
    seconds = max(1, fight_ms // 1000)
    for i in range(seconds):
        for cls in classify(i * 1000 + 500, timeline):
            counts[cls] += 1
    return counts, seconds


def main(path):
    from skills import SkillUsage
    tl = Timeline(SkillUsage(Log(path)))
    print("our spikes (s):   ", [round(s / 1000, 1) for s in tl.our_spikes])
    print("their spikes (s): ", [round(s / 1000, 1) for s in tl.their_spikes])
    top = max(max(tl.out_per_s), max(tl.in_per_s), 1)
    for i, (o, n) in enumerate(zip(tl.out_per_s, tl.in_per_s)):
        mo = "<" if i * 1000 + 500 in tl.our_spikes else " "
        mi = "<" if i * 1000 + 500 in tl.their_spikes else " "
        print(f"{i:4} {'#' * round(20 * o / top):20}{mo} {'#' * round(20 * n / top):20}{mi}")


if __name__ == "__main__":
    main(sys.argv[1])
