"""The logs cut into periods between balance updates: what each period looked like, whether the "why" view's causes hold in
each, and which skills carried it (the meta moves with the patches).

    python tools/import_logs.py                                               data/logs -> data/import/logs.txt
    build/release/frcauses.exe data/causal_import @data/import/logs.txt       (1.5 min for 7,064 logs)
    CAUSAL_DATA=data/causal_import python tools/causal/periods.py [section ...]

Periods: cut at each update with 40+ profession changes that reach WvW (data/patchnotes/changes.csv, tools/patchnotes.py),
at 17:00 UTC on the day (updates go live in the afternoon, UTC). Fights: 10+ a side, 40 s or longer, on WvW maps (Edge
of the Mists and Obsidian Sanctum included: WvW rules; guild halls left out: PvE rules).

Sections (all by default):
    periods   each period: nights, fights, squad and enemy size, fights won (more enemy downs than ally downs)
    comp      squad and enemy specs per period, as a share of the players in the fights
    causes    the view's six ally factors (Causes::kFactorOdds) fitted in each period, and whether they differ
    maps      the same by map (Edge of the Mists, borderlands and EBG, Obsidian Sanctum)
    enemy     why enemies went down, per period
    skills    each period's skills by share of damage and of downs made, both sides, and the biggest moves with the
              patch notes for the skill
"""
import calendar
import csv
import math
import os
import sys
import time

from stats import OUT, clr, load

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from patchnotes_parse import SPECS as SPEC_PROF  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PATCHES = os.path.join(HERE, "..", "..", "data", "patchnotes", "changes.csv")
WVW_MAPS = {38: "Eternal Battlegrounds", 95: "Green Borderlands", 96: "Blue Borderlands", 1099: "Red Borderlands",
            968: "Edge of the Mists", 899: "Obsidian Sanctum"}
VIEW_ODDS = [4.53, 3.05, 2.82, 3.30, 1.80, 1.40]  # the view's odds before the size bands (717 logs, Sept 2026)
MIN_CHANGES = 40
MIN_FIGHTS = 100  # a period with fewer is shown but not compared with (2025-12-09 to 2026-01-13: 2 nights)


def day(ts):
    return time.strftime("%Y-%m-%d", time.gmtime(ts))


def cuts():
    """[(unix time, date, changes)] of the big updates"""
    n = {}
    with open(PATCHES, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["profession"] and r["wvw"] == "1":
                n[r["date"]] = n.get(r["date"], 0) + 1
    out = []
    for d, k in sorted(n.items()):
        if k >= MIN_CHANGES:
            y, m, dd = (int(x) for x in d.split("-"))
            out.append((calendar.timegm((y, m, dd, 17, 0, 0)), d, k))
    return out


def chi2_sf(x, k):
    """P(chi-square with k degrees of freedom > x), Wilson-Hilferty"""
    if k <= 0:
        return 1.0
    z = ((x / k) ** (1 / 3) - (1 - 2 / (9 * k))) / math.sqrt(2 / (9 * k))
    return 0.5 * math.erfc(z / math.sqrt(2))


class Data:
    def __init__(self):
        self.rounds = {r["round"]: r for r in load("rounds.csv")}
        self.players = {(p["round"], p["p"]): p for p in load("players.csv")}
        self.real = {r for r, x in self.rounds.items() if x["squad"] >= 10 and x["enemies"] >= 10 and x["dur"] >= 40000 and x["map"] in WVW_MAPS}
        self.cuts = cuts()
        first = min(self.rounds[r]["server"] for r in self.real)
        last = max(self.rounds[r]["server"] for r in self.real)
        edges = [c for c in self.cuts if first < c[0] <= last]
        self.periods = []  # (name, from, to, update that opened it)
        starts = [(first, None)] + [(c[0], c) for c in edges]
        for i, (s, c) in enumerate(starts):
            e = starts[i + 1][0] if i + 1 < len(starts) else last + 1
            name = f"{day(s)} to {day(e - 1)}"
            self.periods.append((name, s, e, c))
        self.period_of = {}
        for r in self.real:
            t = self.rounds[r]["server"]
            for i, (_, s, e, _) in enumerate(self.periods):
                if s <= t < e:
                    self.period_of[r] = i
                    break

    def in_period(self, i):
        return {r for r, k in self.period_of.items() if k == i}


def section_periods(D):
    print(f"\nPeriods ({len(D.real)} fights with 10+ a side, 40 s+, on WvW maps; {len(D.rounds)} logs read)")
    other = {}
    for r, x in D.rounds.items():
        if x["map"] not in WVW_MAPS:
            other[x["map"]] = other.get(x["map"], 0) + 1
    print(f"  left out, other maps (guild halls and the like): {sum(other.values())} logs, maps {sorted(other.items(), key=lambda kv: -kv[1])[:8]}")
    print(f"  {'period':26s} {'opened by':16s} {'nights':>6s} {'fights':>6s} {'allies':>6s} {'enemies':>7s} {'won %':>6s} {'EotM %':>7s}")
    for i, (name, s, e, c) in enumerate(D.periods):
        rs = D.in_period(i)
        if not rs:
            continue
        X = [D.rounds[r] for r in rs]
        med = lambda v: sorted(v)[len(v) // 2]
        won = sum(1 for x in X if x["edowns"] > x["adowns"])
        opened = f"{c[1]} ({c[2]})" if c else "(data start)"
        print(f"  {name:26s} {opened:16s} {len({x['night'] for x in X}):6d} {len(X):6d} {med([x['squad'] for x in X]):6d} {med([x['enemies'] for x in X]):7d} "
              f"{100 * won / len(X):6.0f} {100 * sum(1 for x in X if x['map'] == 968) / len(X):7.0f}")


def shares(rows, key):
    tot = {}
    for r in rows:
        tot[key(r)] = tot.get(key(r), 0) + 1
    n = max(1, len(rows))
    return {k: 100.0 * v / n for k, v in tot.items()}


def section_comp(D):
    allies = [p for p in D.players.values() if p["round"] in D.real]
    enemies = [e for e in load("enemies.csv") if e["round"] in D.real and e["fought"]]
    for side, rows in (("Ally specs", allies), ("Enemy specs", enemies)):
        print(f"\n{side}: share of the players in the period's fights, % (top 10 overall; change from the period before)")
        per = [shares([r for r in rows if D.period_of.get(r["round"]) == i], lambda r: r["spec"]) for i in range(len(D.periods))]
        overall = shares(rows, lambda r: r["spec"])
        top = [k for k, _ in sorted(overall.items(), key=lambda kv: -kv[1])[:10]]
        print("  " + f"{'period':26s}" + "".join(f"{t[:11]:>12s}" for t in top))
        for i, (name, *_rest) in enumerate(D.periods):
            if not per[i]:
                continue
            print("  " + f"{name:26s}" + "".join(f"{per[i].get(t, 0):12.1f}" for t in top))
        # the biggest moves between periods
        moves = []
        for i in range(1, len(per)):
            if not per[i] or not per[i - 1]:
                continue
            for t in set(per[i]) | set(per[i - 1]):
                d = per[i].get(t, 0) - per[i - 1].get(t, 0)
                moves.append((abs(d), d, t, i))
        moves.sort(reverse=True)
        print("  biggest moves (percentage points):")
        for _, d, t, i in moves[:12]:
            c = D.periods[i][3]
            print(f"    {t:14s} {d:+5.1f}  {per[i - 1].get(t, 0):4.1f} -> {per[i].get(t, 0):4.1f}  at {c[1] if c else '?'}")


def out_front(m, P, kite):
    """Causes::HasFactor F_CcKite / F_RanPast (a raw CSV row: unknown positions are written -1e+05)"""
    mv, sq = float(m["movedin"]), float(m["sqmovedin"])
    if P[(int(m["round"]), int(m["p"]))]["commander"] or min(mv, sq) < -99000 or mv - sq < 300:
        return False
    if kite:
        return sq < -200 and (int(m["cc"]) >= 1 or int(m["hardms"]) > 0)
    return sq >= -200 and m["pulled"] == "0"


def ally_factors(D):
    P = D.players
    return [
        ("CC landed", lambda m: int(m["cc"]) >= 1),
        ("immobilized", lambda m: int(m["immobms"]) > 0),
        ("no protection", lambda m: m["prot"] == "0"),
        ("CC'd during kite", lambda m: out_front(m, P, True)),
        ("ran past the squad", lambda m: out_front(m, P, False)),
        ("hurt coming in", lambda m: 0 <= int(m["hp"]) < 9000),
    ]


def risk_sets(D, name, F):
    """the matched sets, read row by row (the tables run to a million rows): {ev: (round, [(case, features)])}"""
    sets = {}
    with open(os.path.join(OUT, name), newline="") as f:
        for m in csv.DictReader(f):
            r = int(m["round"])
            if r not in D.real:
                continue
            g = sets.setdefault(int(m["ev"]), (r, []))
            g[1].append((m["case"] == "1", tuple(int(bool(fn(m))) for _, fn in F)))
    return sets


def groups_of(sets, rounds):
    out = []
    for r, g in sets.values():
        if r not in rounds:
            continue
        cases = [i for i, (c, _) in enumerate(g) if c]
        if len(cases) != 1 or len(g) < 2:
            continue
        out.append((cases[0], [x for _, x in g]))
    return out


def cell(b, s):
    if abs(b) > 20 or s > 20:
        return "no data".rjust(20)  # the factor never varied in these sets
    return f"{math.exp(b):5.2f} [{math.exp(b - 1.96 * s):4.2f}-{math.exp(b + 1.96 * s):5.2f}]".rjust(20)


def fit_by_period(D, sets, F, title, ref=None):
    print(f"\n{title}")
    names = [n for n, _ in F]
    print("  " + f"{'period':26s} {'downs':>6s}" + "".join(f"{n[:19]:>20s}" for n in names))
    fits = []
    for i, (name, *_rest) in enumerate(D.periods):
        groups = groups_of(sets, D.in_period(i))
        if len(groups) < 200:
            continue
        beta, se, _ = clr(groups, names)
        fits.append((beta, se))
        print("  " + f"{name:26s} {len(groups):6d}" + "".join(cell(b, s) for b, s in zip(beta, se)))
    groups = groups_of(sets, D.real)
    beta, se, _ = clr(groups, names)
    print("  " + f"{'all periods':26s} {len(groups):6d}" + "".join(cell(b, s) for b, s in zip(beta, se)))
    if ref:
        print("  " + f"{'the view (own logs)':26s} {'':6s}" + "".join(f"{v:5.2f}".ljust(15).rjust(20) for v in ref))
    # do the periods differ? Cochran's Q on each factor's log odds
    print("  periods differ (Cochran's Q, p):" + "".join(f"  {n}: {q_test([f[0][k] for f in fits], [f[1][k] for f in fits])}" for k, n in enumerate(names)))
    return beta, se


def q_test(b, s):
    w = [1 / max(x * x, 1e-9) for x in s]
    mean = sum(wi * bi for wi, bi in zip(w, b)) / sum(w)
    q = sum(wi * (bi - mean) ** 2 for wi, bi in zip(w, b))
    p = chi2_sf(q, len(b) - 1)
    return f"{p:.3f}" if p >= 0.001 else "<0.001"


def section_causes(D):
    F = ally_factors(D)
    fit_by_period(D, risk_sets(D, "ally_risk.csv", F), F, "Ally downs: the view's six factors, odds ratio [95% CI] per period", VIEW_ODDS)


def section_maps(D):
    """the same six factors by map: is a period's difference the patch, or where the fights were?"""
    F = ally_factors(D)
    sets = risk_sets(D, "ally_risk.csv", F)
    names = [n for n, _ in F]
    groups = {"Edge of the Mists": {968}, "Borderlands and EBG": {38, 95, 96, 1099}, "Obsidian Sanctum": {899}}
    print("\nAlly downs by map: the view's six factors, odds ratio [95% CI]")
    print("  " + f"{'maps':26s} {'downs':>6s}" + "".join(f"{n[:19]:>20s}" for n in names))
    for g, maps in groups.items():
        rs = {r for r in D.real if D.rounds[r]["map"] in maps}
        gs = groups_of(sets, rs)
        if len(gs) < 200:
            continue
        beta, se, _ = clr(gs, names)
        print("  " + f"{g:26s} {len(gs):6d}" + "".join(cell(b, s) for b, s in zip(beta, se)))


def section_enemy(D):
    F = [
        ("ally CC landed", lambda m: int(m["cc"]) >= 1),
        ("stab stripped", lambda m: m["stabstrip"] != "0"),
        ("boons stripped", lambda m: int(m["strips"]) >= 1),
        ("immobilized", lambda m: int(m["immobms"]) > 0),
        ("hurt coming in", lambda m: 0 <= int(m["hp"]) < 9000),
        ("hit by 5+ allies", lambda m: int(m["allies"]) >= 5),
    ]
    fit_by_period(D, risk_sets(D, "enemy_risk.csv", F), F, "Enemy downs: what allies did, odds ratio [95% CI] per period")


CONDITIONS = {736: "Bleeding", 737: "Burning", 861: "Confusion", 723: "Poison", 19426: "Torment"}
OTHER = "other sources"


def spec_minutes(D):
    """(side, period) -> spec -> player-minutes in the fights: allies by their active time, enemies by the round's length"""
    out = {}
    for p in D.players.values():
        i = D.period_of.get(p["round"])
        if i is not None:
            by = out.setdefault(("a", i), {})
            by[p["spec"]] = by.get(p["spec"], 0) + p["active"] / 60000.0
    with open(os.path.join(OUT, "enemies.csv"), newline="") as f:
        for e in csv.DictReader(f):
            r = int(e["round"])
            i = D.period_of.get(r)
            if i is not None and e["fought"] == "1":
                by = out.setdefault(("e", i), {})
                by[e["spec"]] = by.get(e["spec"], 0) + D.rounds[r]["dur"] / 60000.0
    return out


def fmt_rate(v):
    return "    n/a" if v is None else f"{v:7.1f}"


def section_skills(D):
    names = {}
    with open(os.path.join(OUT, "skill_names.csv"), encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            names[int(r["skill"])] = r["name"]
    with open(PATCHES, encoding="utf-8") as f:
        patches = [p for p in csv.DictReader(f) if p["wvw"] == "1" and p["profession"]]
    minutes = spec_minutes(D)

    def label(spec, sid):
        if spec == "?":
            spec = "siege or NPC"
        if sid in CONDITIONS:
            return f"{spec}: {CONDITIONS[sid]} (condition)"
        n = names.get(sid, str(sid))
        return f"{spec}: {OTHER}" if n.startswith("other sources") or sid == 0 else f"{spec}: {n}"

    # (side, measure) -> period -> "Spec: Skill" -> sum, read row by row (millions of rows)
    MEASURES = {"a": [("damage", "damage to enemy players"), ("downs", "enemy downs made (the last hit before the down)"),
                      ("stab", "stability given to allies")],
                "e": [("damage", "damage to allies"), ("downs", "ally downs made (the last hit before the down)")]}
    sums = {(s, m): [{} for _ in D.periods] for s, ms in MEASURES.items() for m, _ in ms}
    with open(os.path.join(OUT, "skills.csv"), newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            i = D.period_of.get(int(r["round"]))
            if i is None:
                continue
            side = r["side"]
            k = None
            for m, _ in MEASURES[side]:
                v = float(r[m])
                if v:
                    if k is None:
                        k = label(r["spec"], int(r["skill"]))
                    by = sums[(side, m)][i]
                    by[k] = by.get(k, 0) + v

    def rate_of(rate, i, k):
        """the skill's rate in period i: 0 when the spec played and the skill did nothing, None when nobody played the spec"""
        if k in rate[i]:
            return rate[i][k]
        return 0.0 if minutes.get((side, i), {}).get(k.split(": ", 1)[0], 0) >= 1 else None

    def spec_share(side, i, spec):
        by = minutes.get((side, i), {})
        return 100.0 * by.get(spec, 0) / max(1e-9, sum(by.values()))

    for side, what in (("a", "Ally skills"), ("e", "Enemy skills on allies")):
        for measure, title in MEASURES[side]:
            per, rate = [], []
            for i, by in enumerate(sums[(side, measure)]):
                tot = sum(by.values())
                per.append({k: 100.0 * v / tot for k, v in by.items()} if tot else {})
                mins = minutes.get((side, i), {})
                # per player-minute of the spec; none for siege and NPCs (no players to count)
                rate.append({k: v / mins[k.split(": ", 1)[0]] for k, v in by.items() if mins.get(k.split(": ", 1)[0], 0) >= 1})
            print(f"\n{what}, share of {title}, % (top 8 in each period; '{OTHER}': traits, relics, sigils, runes, combos)")
            for i, (name, *_rest) in enumerate(D.periods):
                if not per[i]:
                    continue
                top = [kv for kv in sorted(per[i].items(), key=lambda kv: -kv[1]) if not kv[0].endswith(OTHER)][:8]
                other = sum(v for k, v in per[i].items() if k.endswith(OTHER))
                print(f"  {name}: " + "; ".join(f"{k} {v:.1f}" for k, v in top) + (f"  [{OTHER} {other:.0f}]" if other >= 1 else ""))
            # moves between a period and the one before it with enough fights (a 2-night period is shown, not compared with)
            big = [i for i in range(len(D.periods)) if per[i] and len(D.in_period(i)) >= MIN_FIGHTS]
            moves = []
            for a, b in zip(big, big[1:]):
                for k in set(per[b]) | set(per[a]):
                    if k.endswith(OTHER):
                        continue
                    d = per[b].get(k, 0) - per[a].get(k, 0)
                    moves.append((abs(d), d, k, a, b))
            moves.sort(reverse=True)
            print(f"  biggest moves: share; the spec's share of the players; the skill's {title.split(' (')[0]} per player-minute; the updates' notes for the spec")
            for _, d, k, a, b in moves[:10]:
                spec, skill = k.split(": ", 1)
                prof = SPEC_PROF.get(spec, spec)
                notes = [p for p in patches if day(D.periods[a][1]) < p["date"] <= day(D.periods[b][1]) and (p["spec"] == spec or (p["profession"] == prof and not p["spec"]))]
                # the notes most likely behind it first: this skill, WvW-only, with numbers; bug fixes last
                score = lambda p: (3 * (p["skill"] == skill.replace(" (condition)", "")) + 2 * ("WvW" in p["modes"]) + bool(p["from"])
                                   - 2 * p["text"].startswith("Fixed"))
                shown = sorted(notes, key=score, reverse=True)[:2]
                note = f"{len(notes)} notes" + (": " + " | ".join(f"[{p['skill']}] {p['text'][:90]}" for p in shown) if shown else "")
                at = ", ".join(D.periods[j][3][1] for j in range(a + 1, b + 1) if D.periods[j][3])
                print(f"    {k:44s} {d:+5.1f} ({per[a].get(k, 0):4.1f} -> {per[b].get(k, 0):4.1f})  players {spec_share(side, a, spec):4.1f} -> {spec_share(side, b, spec):4.1f}%  "
                      f"rate {fmt_rate(rate_of(rate, a, k))} -> {fmt_rate(rate_of(rate, b, k))}  at {at}; {note}")


if __name__ == "__main__":
    D = Data()
    want = sys.argv[1:] or ["periods", "comp", "causes", "maps", "enemy", "skills"]
    for s in want:
        globals()["section_" + s](D)
