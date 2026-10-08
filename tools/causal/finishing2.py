"""Which time-equal measure decides an enemy down's fate: damage in its first 1.5 s, or allies hitting it then, held against
the enemy's own players near it (their revivers) and ours."""
import math
from stats import load, logit
rounds = {r["round"]: r for r in load("rounds.csv")}
real = {r for r, x in rounds.items() if x["squad"] >= 10 and x["enemies"] >= 10 and x["dur"] >= 40000}
won = {r for r in real if rounds[r]["edowns"] > rounds[r]["adowns"]}
lost = {r for r in real if rounds[r]["edowns"] < rounds[r]["adowns"]}
case = {m["ev"]: m for m in load("enemy_risk.csv") if m["case"] and m["round"] in real}
D = [d for d in load("enemy_downs.csv") if d["round"] in real and not d["rallied"] and not d["endoflog"] and d["ev"] in case]
print(len(D), "enemy downs (rallies and the log's end left out)")
def q(v):
    v = sorted(v); return (v[len(v)//4], v[len(v)//2], v[3*len(v)//4]) if v else None
for label, S in (("won", won), ("lost", lost)):
    s = [d for d in D if d["round"] in S]
    print(f"{label:5s}: allies hitting in the first 1.5 s {q([d['hittersearly'] for d in s])}; damage per ally then {q([d['cleaveearly'] / 1000 / d['hittersearly'] for d in s if d['hittersearly']])} k; "
          f"their players near the down {q([case[d['ev']]['enemiesnear'] for d in s])}; ours near {q([case[d['ev']]['alliesnear'] for d in s])}")
F = ["1", "damage 1.5 s: 10-20k", "damage 1.5 s: 20k+", "allies on it 1.5 s: 5-9", "allies on it 1.5 s: 10+", "their players near 10+", "our players near 10+"]
rows = []
for d in D:
    c = case[d["ev"]]
    x = [1, int(10000 <= d["cleaveearly"] < 20000), int(d["cleaveearly"] >= 20000), int(5 <= d["hittersearly"] <= 9), int(d["hittersearly"] >= 10),
         int(c["enemiesnear"] >= 10), int(c["alliesnear"] >= 10)]
    rows.append((d["died"], x))
beta, se = logit(rows, F)
print("\nDied, all at once (logistic):")
for n, b, s in zip(F, beta, se):
    print(f"  {n:28s} OR {math.exp(b):6.2f}  [{math.exp(b - 1.96 * s):6.2f}-{math.exp(b + 1.96 * s):6.2f}]")
def rate(sel):
    s = [d for d in D if sel(d)]
    return f"{len(s):5d}, died {100 * sum(d['died'] for d in s) / max(1, len(s)):4.1f}%"
print("\nBy allies hitting it in its first 1.5 s:")
for lo, hi in ((0, 1), (1, 3), (3, 5), (5, 8), (8, 11), (11, 99)):
    print(f"  {lo}-{hi - 1 if hi < 99 else '+'}: {rate(lambda d, lo=lo, hi=hi: lo <= d['hittersearly'] < hi)}")
