"""Why enemy downs got back up: too little cleave, or picked up too fast for damage to add up? Each enemy down ends one of
three ways: it died; it rallied (an ally it had hit died
within 0.4 s of it getting up); or it was picked up (revives and skills: the enemy's aren't in the log, so the rest).

    python tools/causal/finishing.py [stamp]     (a round's stamp, e.g. 20261006-220040: its downs too)

1. Is "rallied" real? Enemy get-ups against ally deaths at the same moment, and at moments shifted 2 to 8 s (chance).
2. How enemy downs ended, won rounds against lost.
3. Cleave with time held equal: damage in each down's first 1.5 s (downs that lasted that long), and allies hitting it.
4. How long the picked-up ones were down.
"""
import sys
from stats import load


def q(v):
    v = sorted(v)
    return (v[len(v) // 4], v[len(v) // 2], v[3 * len(v) // 4]) if v else (0, 0, 0)


rounds = {r["round"]: r for r in load("rounds.csv")}
real = {r for r, x in rounds.items() if x["squad"] >= 10 and x["enemies"] >= 10 and x["dur"] >= 40000 and x["adowns"] + x["edowns"] >= 5}
won = {r for r in real if rounds[r]["edowns"] > rounds[r]["adowns"]}
lost = {r for r in real if rounds[r]["edowns"] < rounds[r]["adowns"]}
ED = [d for d in load("enemy_downs.csv") if d["round"] in real]
AD = [d for d in load("ally_downs.csv") if d["round"] in real]

# 1. coincidence of enemy get-ups with ally deaths, at offset 0 and shifted
deaths = {}
for d in AD:
    if d["died"]:
        deaths.setdefault(d["round"], []).append(d["t"] + d["downms"])
ups = [(d["round"], d["t"] + d["downms"]) for d in ED if not d["died"] and not d["endoflog"]]
print(f"1. Enemy get-ups: {len(ups)}. Share with an ally death within 0.4 s, at the moment and shifted (chance):")
for shift in (0, 2000, -2000, 4000, -4000, 8000, -8000):
    n = sum(1 for r, t in ups if any(abs(x - (t + shift)) <= 400 for x in deaths.get(r, [])))
    print(f"   shift {shift / 1000:+5.1f} s: {100 * n / max(1, len(ups)):5.1f}%")
print(f"   with the hit check (it had hit that ally in the 15 s before): {100 * sum(d['rallied'] for d in ED if not d['died'] and not d['endoflog']) / max(1, len(ups)):.1f}%")

# 2. how enemy downs ended
def ends(S, label):
    D = [d for d in ED if d["round"] in S]
    n = len(D)
    died = sum(d["died"] for d in D)
    rallied = sum(1 for d in D if d["rallied"])
    eol = sum(1 for d in D if d["endoflog"] and not d["died"])
    picked = n - died - rallied - eol
    print(f"   {label:5s} {n:5d} downs: died {100 * died / n:4.1f}%  rallied {100 * rallied / n:4.1f}%  picked up {100 * picked / n:4.1f}%  still down at the end {100 * eol / n:4.1f}%")
print("\n2. How enemy downs ended")
ends(won, "won")
ends(lost, "lost")

# 3. cleave with time held equal
print("\n3. Allies' damage on an enemy down, the same 1.5 s for each (downs that lasted 1.5 s or more): median [quartiles]")
for label, S in (("won", won), ("lost", lost)):
    D = [d for d in ED if d["round"] in S and d["downms"] >= 1500]
    print(f"   {label:5s} {len(D):5d} downs: first 1.5 s {q([d['cleaveearly'] / 1000 for d in D])} k; allies hitting {q([d['hitters'] for d in D])}; "
          f"first hit after {q([d['firsthit'] for d in D if d['firsthit'] >= 0])} ms; never hit {100 * sum(1 for d in D if d['firsthit'] < 0) / max(1, len(D)):.0f}%")
print("   and what the first 1.5 s decided, all rounds (downs that lasted 1.5 s+, rallies left out):")
D = [d for d in ED if d["downms"] >= 1500 and not d["rallied"] and not d["endoflog"]]
for lo, hi in ((0, 5000), (5000, 10000), (10000, 20000), (20000, 30000), (30000, 10 ** 9)):
    s = [d for d in D if lo <= d["cleaveearly"] < hi]
    print(f"   {lo // 1000:3d}k-{'' if hi > 10 ** 8 else str(hi // 1000) + 'k':4s} in the first 1.5 s: {len(s):5d} downs, died {100 * sum(d['died'] for d in s) / max(1, len(s)):4.1f}%")
for lo, hi in ((0, 3), (3, 6), (6, 10), (10, 99)):
    s = [d for d in D if lo <= d["hitters"] < hi]
    print(f"   {lo}-{hi - 1 if hi < 99 else '+'} allies hitting it: {len(s):5d} downs, died {100 * sum(d['died'] for d in s) / max(1, len(s)):4.1f}%")

# 4. how long the picked-up ones were down
print("\n4. Time down before the enemy got them up (picked up, not rallied): median [quartiles] ms")
for label, S in (("won", won), ("lost", lost)):
    D = [d for d in ED if d["round"] in S and not d["died"] and not d["rallied"] and not d["endoflog"]]
    print(f"   {label:5s} {len(D):5d}: {q([d['downms'] for d in D])}")
for label, S in (("won", won), ("lost", lost)):
    D = [d for d in ED if d["round"] in S and d["died"]]
    print(f"   died, {label:5s} {len(D):5d}: down {q([d['downms'] for d in D])} ms before dying")

# the same question for allies: our downs that rallied when an enemy died
print("\n5. Ally downs that rallied (an enemy they had hit died within 0.4 s of them getting up):")
for label, S in (("won", won), ("lost", lost)):
    D = [d for d in AD if d["round"] in S]
    print(f"   {label:5s} {len(D):5d} downs: rallied {100 * sum(d['rallied'] for d in D) / max(1, len(D)):.1f}%")

if len(sys.argv) > 1:
    stamp = sys.argv[1]
    r = [x for x in rounds if rounds[x]["stamp"] == stamp]
    if r:
        r = r[0]
        D = [d for d in load("enemy_downs.csv") if d["round"] == r]
        print(f"\n{stamp}: {len(D)} enemy downs")
        for d in D:
            how = "died" if d["died"] else "RALLIED" if d["rallied"] else "end of log" if d["endoflog"] else "picked up"
            print(f"   {d['t'] / 1000:6.1f}s  down {d['downms'] / 1000:4.1f}s  {how:10s} cleave {d['cleave'] / 1000:5.1f}k  first 1.5 s {d['cleaveearly'] / 1000:5.1f}k  "
                  f"allies hitting {d['hitters']:2d}  first hit {d['firsthit'] / 1000 if d['firsthit'] >= 0 else '-'}")
