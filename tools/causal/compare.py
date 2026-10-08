"""The view's causes compared across log sets and fight sizes (15v15 is the GvG standard; 20v20, 25v25 and bigger fights
are common too).

    python tools/import_logs.py data/logs "<own log folder>"                 every set, each fight once
    build/release/frcauses.exe data/causal_all @data/import/logs.txt
    CAUSAL_DATA=data/causal_all python tools/causal/compare.py [sources] [sizes] [refs] [since=2026-07-14]

The model is analyse.py's (the view's six causes held against aegis, resistance, few allies near and dodging); fights
with 10+ a side, 40 s or longer, on WvW maps. A round's set is told by its log's path (rounds.csv "file").
    sources   each set alone, the total, the total without each set
    sizes     by the squad's size: 10-17 (15v15), 18-22 (20v20), 23-27 (25v25), 28-39, 40+; each set's own sizes too
    refs      won fights' values per size (what the view's "won fights" column would show)
"""
import calendar
import collections
import csv
import math
import os
import sys

from stats import OUT, clr

WVW_MAPS = {38, 95, 96, 1099, 968, 899}
VIEW = ["CC landed", "immobilized", "no protection", "CC'd during kite", "ran past the squad", "hurt coming in"]
ADJ = ["no aegis", "no resistance", "few allies near", "didn't dodge"]
VIEW_ODDS = [4.53, 3.05, 2.82, 3.30, 1.80, 1.40]  # the view's odds from one squad's logs alone, before the size bands
SIZES = [(10, 17, "10-17 (15v15)"), (18, 22, "18-22 (20v20)"), (23, 27, "23-27 (25v25)"), (28, 39, "28-39"), (40, 999, "40+")]


ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
norm = lambda p: os.path.normcase(os.path.abspath(os.path.join(ROOT, p))).replace("\\", "/")
# whose logs: the import's manifest names each log's player folder ("source"; tools/import_logs.py)
MANIFEST = {}
if os.path.exists(os.path.join(ROOT, "data", "import", "manifest.csv")):
    with open(os.path.join(ROOT, "data", "import", "manifest.csv"), newline="", encoding="utf-8") as f:
        for m in csv.DictReader(f):
            if m.get("file") and m.get("source"):
                MANIFEST[norm(m["file"])] = m["source"]


def source_of(path):
    if norm(path) in MANIFEST:
        return MANIFEST[norm(path)]
    p = path.replace("\\", "/").lower()  # tables from before the manifest named players
    return "own" if "cbtlogs" in p else "other"


def size_of(n):
    for lo, hi, name in SIZES:
        if lo <= n <= hi:
            return name
    return None


rounds = {}
with open(os.path.join(OUT, "rounds.csv"), newline="", encoding="utf-8") as f:
    for r in csv.DictReader(f):
        rounds[int(r["round"])] = r
# since=YYYY-MM-DD: fights from that day on only (17:00 UTC, as the updates go live)
SINCE = 0
for a in sys.argv[1:]:
    if a.startswith("since="):
        y, mo, d = (int(x) for x in a[6:].split("-"))
        SINCE = calendar.timegm((y, mo, d, 17, 0, 0))
real = {k for k, r in rounds.items() if int(r["squad"]) >= 10 and int(r["enemies"]) >= 10 and int(r["dur"]) >= 40000
        and int(r.get("map") or 38) in WVW_MAPS and int(r.get("server") or 0) >= SINCE}
src = {k: source_of(r.get("file", "")) for k, r in rounds.items()}
cmdr = set()
with open(os.path.join(OUT, "players.csv"), newline="") as f:
    for p in csv.DictReader(f):
        if p["commander"] == "1":
            cmdr.add((int(p["round"]), int(p["p"])))


def features(m, r):
    mv, sq = float(m["movedin"]), float(m["sqmovedin"])
    out = (r, int(m["p"])) not in cmdr and min(mv, sq) > -99000 and mv - sq >= 300
    cc, hard = int(m["cc"]) >= 1, int(m["hardms"]) > 0
    return (cc, int(m["immobms"]) > 0, m["prot"] == "0", out and sq < -200 and (cc or hard), out and sq >= -200 and m["pulled"] == "0",
            0 <= int(m["hp"]) < 9000, m["aegis"] == "0", m["resist"] == "0",
            int(m["alliesnear"]) <= 3 and float(m["tosquad"]) >= 0, m["dodges"] == "0")


# downs after allies stopped fighting (the round was decided: Causes::AllyDown::AfterStop) are left out, as in the view
after = set()
with open(os.path.join(OUT, "ally_downs.csv"), newline="") as f:
    for d in csv.DictReader(f):
        if d.get("afterstop") == "1":
            after.add(int(d["ev"]))
sets = {}
with open(os.path.join(OUT, "ally_risk.csv"), newline="") as f:
    for m in csv.DictReader(f):
        r = int(m["round"])
        if r in real and int(m["ev"]) not in after:
            g = sets.setdefault(int(m["ev"]), (r, []))
            g[1].append((m["case"] == "1", tuple(int(x) for x in features(m, r))))


def fit(rs):
    groups = []
    for r, g in sets.values():
        if r not in rs:
            continue
        cases = [i for i, (c, _) in enumerate(g) if c]
        if len(cases) == 1 and len(g) >= 2:
            groups.append((cases[0], [list(x) for _, x in g]))
    if len(groups) < 150:
        return len(groups), None
    beta, se, _ = clr(groups, VIEW + ADJ)
    return len(groups), list(zip(beta[:len(VIEW)], se[:len(VIEW)]))


def cell(bs):
    b, s = bs
    if abs(b) > 20 or s > 20:
        return "n/a".rjust(18)
    return f"{math.exp(b):5.2f} [{math.exp(b - 1.96 * s):4.2f}-{math.exp(b + 1.96 * s):5.2f}]".rjust(18)


def header(first):
    print("  " + f"{first:30s} {'fights':>6s} {'downs':>6s}" + "".join(f"{n[:17]:>18s}" for n in VIEW))


def line(name, rs):
    n, res = fit(rs)
    if res is None:
        print("  " + f"{name:30s} {len(rs):6d} {n:6d}  (too few downs)")
        return None
    print("  " + f"{name:30s} {len(rs):6d} {n:6d}" + "".join(cell(x) for x in res))
    return res


def differ(results):
    """Cochran's Q per factor across the given fits: do they differ more than chance would?"""
    out = []
    for k, n in enumerate(VIEW):
        b = [r[k][0] for r in results if r]
        s = [r[k][1] for r in results if r]
        if len(b) < 2:
            continue
        w = [1 / max(x * x, 1e-9) for x in s]
        mean = sum(wi * bi for wi, bi in zip(w, b)) / sum(w)
        q = sum(wi * (bi - mean) ** 2 for wi, bi in zip(w, b))
        k_ = len(b) - 1
        z = ((q / k_) ** (1 / 3) - (1 - 2 / (9 * k_))) / math.sqrt(2 / (9 * k_))
        p = 0.5 * math.erfc(z / math.sqrt(2))
        out.append(f"{n}: {p:.3f}" if p >= 0.001 else f"{n}: <0.001")
    print("  differ (Cochran's Q, p): " + "; ".join(out))


def section_sources():
    print(f"\nEach set alone and together: odds ratio [95% CI] ({len(real)} fights in all)")
    header("set")
    names = sorted({src[r] for r in real})
    res = [line(n, {r for r in real if src[r] == n}) for n in names]
    differ(res)
    line("all together", real)
    for n in names:
        line(f"all without {n}", {r for r in real if src[r] != n})
    print("  " + f"{'the view now (own logs)':30s} {'':6s} {'':6s}" + "".join(f"{v:5.2f}".ljust(13).rjust(18) for v in VIEW_ODDS))


def section_sizes():
    print("\nBy the squad's size (allies in the fight): odds ratio [95% CI]")
    header("allies")
    res = []
    for lo, hi, name in SIZES:
        rs = {r for r in real if lo <= int(rounds[r]["squad"]) <= hi}
        if rs:
            enemies = sorted(int(rounds[r]["enemies"]) for r in rs)
            res.append(line(f"{name}, enemies ~{enemies[len(enemies) // 2]}", rs))
    differ(res)
    # the sets differ in size (one set's squads are bigger): sizes inside each set apart
    for n in sorted({src[r] for r in real}):
        print(f"\n  inside {n} only:")
        res = []
        for lo, hi, name in SIZES:
            rs = {r for r in real if src[r] == n and lo <= int(rounds[r]["squad"]) <= hi}
            if len(rs) >= 20:
                res.append(line(name, rs))
        differ(res)


def section_refs():
    S = {}
    with open(os.path.join(OUT, "summary.csv"), newline="") as f:
        for s in csv.DictReader(f):
            S[int(s["round"])] = s
    print("\nWon fights by size (5+ downs): per 10 allies per minute, median / upper quartile (the view's 'won fights' column)")
    keys = ["downs", "w_cc", "w_immob", "w_noprot", "w_kite", "w_ranpast", "w_hurt", "edowns"]
    print("  " + f"{'allies':16s} {'won':>4s} {'lost':>4s}" + "".join(f"{k:>13s}" for k in keys))
    for lo, hi, name in SIZES:
        rs = [r for r in real if lo <= int(rounds[r]["squad"]) <= hi and r in S
              and int(rounds[r]["adowns"]) + int(rounds[r]["edowns"]) >= 5]
        won = [r for r in rs if int(rounds[r]["edowns"]) > int(rounds[r]["adowns"])]
        lost = [r for r in rs if int(rounds[r]["edowns"]) < int(rounds[r]["adowns"])]
        if len(won) < 10:
            continue
        scale = lambda r: int(rounds[r]["squad"]) / 10 * int(rounds[r]["dur"]) / 60000

        def mq(k):
            v = sorted(float(S[r][k]) / scale(r) for r in won)
            return f"{v[len(v) // 2]:5.2f}/{v[3 * len(v) // 4]:5.2f}"
        print("  " + f"{name:16s} {len(won):4d} {len(lost):4d}" + "".join(f"{mq(k):>13s}" for k in keys))


if __name__ == "__main__":
    want = [a for a in sys.argv[1:] if not a.startswith("since=")] or ["sources", "sizes", "refs"]
    for s in want:
        globals()["section_" + s]()
