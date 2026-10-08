"""src/CauseRefs.inc: the "why" view's numbers for each fight size (Causes::SizeBand): a round is judged against fights of
its own size, 15v15 (the GvG standard) to 40v40 and up.

    python tools/import_logs.py data/logs "<own log folder>"                 every player's logs, each fight once
    build/release/frcauses.exe data/causal_all @data/import/logs.txt
    CAUSAL_DATA=data/causal_all python tools/causal/gen_refs.py [since=2026-07-14]
Run it twice after the odds change (build and extract in between): what a cause cost in a won fight uses the odds.

Per band of allies in the round (10-17, 18-22, 23-27, 28-39, 40+):
  odds        the view's six causes, the model of analyse.py (held against aegis, resistance, few allies near, dodging),
              every period (the causes held across the patches: periods.py), fights with 10+ a side, 40 s+, WvW maps
  won fights  each line's median and worse quartile in won fights, lost fights' median: since the last big balance
              update (`since`, default 2026-07-14), as the meta and the log format are now; fights with 5+ downs in all
Standard library only.
"""
import calendar
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0]] + [a for a in sys.argv[1:]]  # compare.py reads since= for its own fits; ours are every period
SINCE_ARG = next((a[6:] for a in sys.argv[1:] if a.startswith("since=")), "2026-07-14")
sys.argv = [sys.argv[0]]
import compare as C  # noqa: E402  (loads the tables: every period)

OUT_INC = os.path.join(HERE, "..", "..", "src", "CauseRefs.inc")
BANDS = [(10, 17, "15v15"), (18, 22, "20v20"), (23, 27, "25v25"), (28, 39, "30v30"), (40, 9999, "40v40+")]  # allies in the round
y, mo, d = (int(x) for x in SINCE_ARG.split("-"))
SINCE = calendar.timegm((y, mo, d, 17, 0, 0))

R = C.rounds
S = {}
with open(os.path.join(C.OUT, "summary.csv"), newline="") as f:
    for s in csv.DictReader(f):
        S[int(s["round"])] = {k: float(v) for k, v in s.items()}
ED = {}
with open(os.path.join(C.OUT, "enemy_downs.csv"), newline="") as f:
    for e in csv.DictReader(f):
        if e["endoflog"] == "0":
            ED.setdefault(int(e["round"]), []).append(e)


def q(v):
    v = sorted(v)
    return (v[len(v) // 4], v[len(v) // 2], v[3 * len(v) // 4]) if v else (0.0, 0.0, 0.0)


def middle(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def scale(r):
    return int(R[r]["squad"]) / 10 * int(R[r]["dur"]) / 60000


def hitters(r):
    d = ED.get(r, [])
    return middle([int(x["hittersearly"]) for x in d]) if len(d) >= 2 else None


def rallied(r):
    return sum(int(x["rallied"]) for x in ED.get(r, [])) / scale(r)


# the view's lines in UiStruggle.cpp's LineId order: (kind, value of a round or None, cost of a round)
per = lambda k: (lambda r: S[r][k] / scale(r))
rate = lambda k, pushes=0: (lambda r: S[r][k] if S[r][k] >= 0 and S[r]["apushes"] >= pushes else None)
LINES = [
    ("count", per("w_cc"), per("c_cc")), ("count", per("w_immob"), per("c_immob")), ("count", per("w_noprot"), per("c_noprot")),
    ("count", per("w_kite"), per("c_kite")), ("count", per("w_ranpast"), per("c_ranpast")), ("count", per("w_hurt"), per("c_hurt")),
    ("count", per("ranout"), None), ("count", per("unused"), None), ("count", per("fast"), None),
    ("rate", rate("cclanded"), None), ("rate", rate("focus", 1), None), ("rate", rate("ccbefore", 2), None),
    ("rate", hitters, None), ("count", rallied, None),
]


def ref(kind, won, lost, cost=None):
    w = [x for x in won if x is not None]
    l = [x for x in lost if x is not None]
    qw, ql = q(w), q(l)
    worse = qw[2] if kind == "count" else qw[0]
    c = q([x for x in cost if x is not None])[1] if cost else 0.0
    return (qw[1], worse, c, ql[1])


def share(rs, a, b):
    return [S[r][a] / S[r][b] for r in rs if S[r][b] >= 2]


def pooled(rs, a, b):
    return sum(S[r][a] for r in rs) / max(1.0, sum(S[r][b] for r in rs))


def g(v):
    return f"{v:.4g}"


rows = []
for lo, hi, name in BANDS:
    odds_rounds = {r for r in C.real if lo <= int(R[r]["squad"]) <= hi}
    n, fit = C.fit(odds_rounds)
    odds = [2.718281828 ** b for b, _ in fit]
    rs = [r for r in C.real if lo <= int(R[r]["squad"]) <= hi and int(R[r]["server"]) >= SINCE and r in S
          and int(R[r]["adowns"]) + int(R[r]["edowns"]) >= 5]
    won = [r for r in rs if int(R[r]["edowns"]) > int(R[r]["adowns"])]
    lost = [r for r in rs if int(R[r]["edowns"]) < int(R[r]["adowns"])]
    lines = [ref(kind, [fn(r) for r in won], [fn(r) for r in lost], [cf(r) for r in won] if cf else None) for kind, fn, cf in LINES]
    downs = ref("count", [S[r]["downs"] / scale(r) for r in won], [S[r]["downs"] / scale(r) for r in lost])
    edowns = ref("rate", [S[r]["edowns"] / scale(r) for r in won], [S[r]["edowns"] / scale(r) for r in lost])
    died = (pooled(won, "deaths", "downs"), q(share(won, "deaths", "downs"))[2], 0.0, q(share(lost, "deaths", "downs"))[1])
    edied = (pooled(won, "edeaths", "edowns"), q(share(won, "edeaths", "edowns"))[0], 0.0, q(share(lost, "edeaths", "edowns"))[1])
    rows.append((hi, name, len(odds_rounds), len(won), odds, lines, downs, died, edowns, edied, n, len(lost)))
    print(f"{name:6s} odds from {len(odds_rounds)} fights ({n} downs): " + " ".join(f"{o:.2f}" for o in odds)
          + f"; won-fight values from {len(won)} won, {len(lost)} lost fights since {SINCE_ARG}")

lr = lambda t: "{" + ", ".join(g(x) for x in t) + "}"
with open(OUT_INC, "w", encoding="utf-8", newline="\n") as f:
    f.write("// Generated by tools/causal/gen_refs.py from every player's logs at hand (data/causal_all). Do not edit.\n")
    f.write(f"// Odds: every period; won-fight values: fights since {SINCE_ARG}. Lines in UiStruggle.cpp's LineId order;\n")
    f.write("// each {won median, won worse quartile, cost in a won fight, lost median}; heads: downs, died, enemy downs, enemy died.\n")
    f.write("const SizeBand kBands[] = {\n")
    for hi, name, nf, nw, odds, lines, downs, died, edowns, edied, _, _ in rows:
        f.write(f"\t{{{min(hi, 9999)}, \"{name}\", {nf}, {nw}, {{{', '.join(g(o) for o in odds)}}},\n")
        f.write("\t\t{{" + ", ".join(lr(t) for t in lines) + "}},\n")
        f.write(f"\t\t{lr(downs)}, {lr(died)}, {lr(edowns)}, {lr(edied)}}},\n")
    f.write("};\n")
print(f"-> {os.path.normpath(OUT_INC)}")
