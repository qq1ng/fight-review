"""The numbers behind the "why" view (src/UiStruggle.cpp, src/Causes.cpp), from frcauses' CSV tables.

Usage:
    build/release/frcauses.exe data/causal "<log folder>"/*.zevtc      (17 s for 717 logs)
    python tools/causal/analyse.py [section ...]                        (CAUSAL_DATA=<dir> to read another folder)

Sections (all by default):
    ally      why allies went down: each down against the allies under the same fire at that moment who stayed up
              (conditional logistic regression on the matched sets): Causes::kFactorOdds
    checks    the same with how hard each ally was targeted held fixed, and on each half of the nights apart
    enemy     why enemies went down, the same way (the hovers' "2.4 times as often", "5 times as often")
    revives   downs that died, by revive tools ready within reach (generous and strict reading)
    refs      won and lost fights' values for every line of the view (kDefs and the part heads in UiStruggle.cpp)

Fights counted: 10+ a side, 40 s or longer ("real fights"); the references also need 5+ downs in all.
Results 2026-10-07 (717 logs, 3 Sept to 6 Oct; 6,275 ally downs, 9,447 enemy downs, 550 rounds): see notes/TODO.md #1.
finishing.py and finishing2.py: why enemy downs got back up (rallies, allies on the down, time held equal).
"""
import math
import sys

from stats import clr, load, logit, mh_or, fmt_or, strata


def real_rounds(rounds, downs=0):
    return {r for r, x in rounds.items() if x["squad"] >= 10 and x["enemies"] >= 10 and x["dur"] >= 40000 and x["adowns"] + x["edowns"] >= downs}


def matched(risk, features, keep=lambda m: True):
    """risk rows -> CLR groups: (case index, feature vectors), one per down with a case and 1+ others"""
    groups = []
    for ev, g in strata(risk).items():
        g = [m for m in g if m["case"] or keep(m)]
        cases = [i for i, m in enumerate(g) if m["case"]]
        if len(cases) != 1 or len(g) < 2 or not keep(g[cases[0]]):
            continue
        groups.append((cases[0], [[f(m) for _, f in features] for m in g]))
    return groups


def show(features, groups, title):
    beta, se, ll = clr(groups, [n for n, _ in features])
    print(f"\n{title}: {len(groups)} matched sets")
    for (n, _), b, s in zip(features, beta, se):
        print(f"  {n:30s} OR {math.exp(b):6.2f}  [{math.exp(b - 1.96 * s):6.2f}-{math.exp(b + 1.96 * s):6.2f}]")


def out_front(m, players, kite):
    """Causes::HasFactor F_CcKite / F_RanPast: 300+ further toward the enemy than the squad's middle went in the 3 s before
    (positions known, not the tag). The squad's middle went 200+ back (a kite) and they were CC'd, rooted or slowed: CC'd
    during the kite; otherwise, not pulled or taunted: ran past the squad"""
    if players[(m["round"], m["p"])]["commander"] or min(m["movedin"], m["sqmovedin"]) < -99000 or m["movedin"] - m["sqmovedin"] < 300:
        return 0
    if kite:
        return int(m["sqmovedin"] < -200 and (m["cc"] >= 1 or m["hardms"] > 0))
    return int(m["sqmovedin"] >= -200 and m["pulled"] == 0)


def ally_features(players):
    # the view's factors, plus what they're held against (aegis, resistance, few allies near, dodging). Strips and the
    # number of enemies hitting are left out: they are how the enemy's spike works (see "checks")
    return [
        ("CC landed", lambda m: int(m["cc"] >= 1)),
        ("immobilized", lambda m: int(m["immobms"] > 0)),
        ("no protection", lambda m: int(not m["prot"])),
        ("no aegis", lambda m: int(not m["aegis"])),
        ("no resistance", lambda m: int(not m["resist"])),
        ("hurt coming in (<90%)", lambda m: int(0 <= m["hp"] < 9000)),
        ("CC'd during kite", lambda m: out_front(m, players, True)),
        ("ran past the squad", lambda m: out_front(m, players, False)),
        ("few allies near (known pos.)", lambda m: int(m["alliesnear"] <= 3 and m["tosquad"] >= 0)),
        ("didn't dodge", lambda m: int(m["dodges"] == 0)),
    ]


def section_ally(rounds, players, real):
    # downs after allies stopped fighting (the round was decided) are left out, as in the view
    after = {d["ev"] for d in load("ally_downs.csv") if d.get("afterstop") == 1}
    risk = [r for r in load("ally_risk.csv") if r["round"] in real and r["ev"] not in after]
    F = ally_features(players)
    show(F, matched(risk, F), "Ally downs, the view's factors (Causes::kFactorOdds: CC, immobilized, no protection, CC'd during kite, ran past, hurt)")
    cases = [m for m in risk if m["case"] and m["cc"] >= 1]
    print(f"  CC'd then down: {len(cases)}; stability stripped in the 4 s before: {100 * sum(m['stabstrip'] for m in cases) / max(1, len(cases)):.0f}%; "
          f"median CC to down {sorted(m['sincecc'] for m in cases)[len(cases) // 2] if cases else -1} ms")
    return risk


def section_checks(rounds, players, real, risk):
    F = ally_features(players)
    T = F + [("targeted by 5-9", lambda m: int(5 <= m["enemies"] <= 9)), ("targeted by 10-14", lambda m: int(10 <= m["enemies"] <= 14)),
             ("targeted by 15+", lambda m: int(m["enemies"] >= 15)), ("boons stripped", lambda m: int(m["strips"] >= 1))]
    show(T, matched(risk, T), "Held against how hard they were targeted (and strips)")
    nights = sorted({rounds[r]["night"] for r in real})
    half = nights[len(nights) // 2]
    show(F, matched([m for m in risk if rounds[m["round"]]["night"] < half], F), f"First half of the nights (< {half})")
    show(F, matched([m for m in risk if rounds[m["round"]]["night"] >= half], F), "Second half")


def section_enemy(real):
    risk = [r for r in load("enemy_risk.csv") if r["round"] in real]
    F = [
        ("ally CC landed", lambda m: int(m["cc"] >= 1)),
        ("their stab blocked ally CC", lambda m: int(m["blocked"] >= 1)),
        ("stab stripped by allies", lambda m: int(m["stabstrip"])),
        ("boons stripped by allies", lambda m: int(m["strips"] >= 1)),
        ("immobilized", lambda m: int(m["immobms"] > 0)),
        ("health <90% entering", lambda m: int(0 <= m["hp"] < 9000)),
        ("invulnerable (to 1.2 s before)", lambda m: int(m["invuln"] >= 1)),
        ("out in front 300+", lambda m: int(m["ahead"] >= 300)),
        ("their allies near <=3 (known)", lambda m: int(m["enemiesnear"] <= 3 and m["totheir"] >= 0)),
        ("hit by 5+ allies", lambda m: int(m["allies"] >= 5)),
        ("hit by 10+ allies", lambda m: int(m["allies"] >= 10)),
    ]
    show(F, matched(risk, F), "Enemy downs, what allies did to them (10+ against fewer than 5: multiply the two 'hit by' rows)")


def section_revives(real):
    D = [d for d in load("ally_downs.csv") if d["round"] in real]

    def rate(sel):
        s = [d for d in D if sel(d)]
        return f"{len(s):5d} downs, died {100 * sum(d['died'] for d in s) / max(1, len(s)):4.1f}%"
    print("\nDowns that died, by revive tools ready within reach")
    for label, key in (("generous (1500, alacrity)", "toolsready"), ("strict (1200, full recharge)", "toolsstrict")):
        print(f"  {label}:")
        for k in (0, 1, 2):
            print(f"    {k}  {rate(lambda d, k=k: d[key] == k)}")
        print(f"    3+ {rate(lambda d: d[key] >= 3)}")
    unused = [d for d in D if d["died"] and d["downms"] >= 1500 and d["toolsready"] >= 2]
    if unused:
        print(f"  'not used' deaths (2+ ready, generous): {len(unused)}; still 2+ on the strict reading: {100 * sum(1 for d in unused if d['toolsstrict'] >= 2) / len(unused):.0f}%")
    died = [d for d in D if d["died"]]
    q = sorted(d["downms"] for d in died)
    if q:
        print(f"  time down before dying: median {q[len(q) // 2]} ms; died within 1.5 s: {100 * sum(1 for x in q if x < 1500) / len(q):.0f}% of deaths")


def section_refs(rounds):
    real = real_rounds(rounds, 5)
    S = {s["round"]: s for s in load("summary.csv")}
    won = [r for r in real if rounds[r]["edowns"] > rounds[r]["adowns"] and r in S]
    lost = [r for r in real if rounds[r]["edowns"] < rounds[r]["adowns"] and r in S]
    scale = lambda r: rounds[r]["squad"] / 10 * rounds[r]["dur"] / 60000

    def q(v):
        v = sorted(v)
        return (v[len(v) // 4], v[len(v) // 2], v[3 * len(v) // 4]) if v else (0, 0, 0)

    def auc(f):
        L = [f(r) for r in lost if f(r) is not None]
        W = [f(r) for r in won if f(r) is not None]
        return sum((1 if a > b else 0.5 if a == b else 0) for a in L for b in W) / max(1, len(L) * len(W))
    print(f"\nReferences: {len(real)} rounds, won {len(won)}, lost {len(lost)} (AUC: the chance a lost round shows more than a won one)")
    print("  counts per 10 allies per minute: median [lower - upper quartile]")
    for k in ["downs", "deaths", "w_cc", "w_immob", "w_noprot", "w_kite", "w_ranpast", "w_hurt", "c_cc", "c_immob", "c_noprot", "c_kite", "c_ranpast",
              "c_hurt",
              "fast", "ranout", "unused", "onnobody", "edowns", "edeaths"]:
        f = lambda r, k=k: S[r][k] / scale(r)
        w, l = q([f(r) for r in won]), q([f(r) for r in lost])
        print(f"  {k:9s} AUC {auc(f):4.2f}  won {w[1]:6.3f} [{w[0]:6.3f}-{w[2]:6.3f}]  lost {l[1]:6.3f} [{l[0]:6.3f}-{l[2]:6.3f}]")
    print("  rates as they are")
    for k in ["focus", "cclanded", "ccbefore", "cleave"]:
        f = lambda r, k=k: S[r][k] if S[r][k] != -1 else None
        w = q([f(r) for r in won if f(r) is not None])
        l = q([f(r) for r in lost if f(r) is not None])
        print(f"  {k:9s} AUC {auc(f):4.2f}  won {w[1]:9.3f} [{w[0]:9.3f}-{w[2]:9.3f}]  lost {l[1]:9.3f} [{l[0]:9.3f}-{l[2]:9.3f}]")
    pool = lambda rs, a, b: sum(S[r][a] for r in rs) / max(1, sum(S[r][b] for r in rs))
    print(f"  ally downs that died, pooled: won {pool(won, 'deaths', 'downs'):.3f}, lost {pool(lost, 'deaths', 'downs'):.3f}")
    print(f"  enemy downs that died, pooled: won {pool(won, 'edeaths', 'edowns'):.3f}, lost {pool(lost, 'edeaths', 'edowns'):.3f}")
    # finishing enemy downs, time held equal: allies on a down in its first 1.5 s (the round's middle down), rallies
    ED = {}
    for d in load("enemy_downs.csv"):
        if d["round"] in real and not d["endoflog"]:
            ED.setdefault(d["round"], []).append(d)
    med = lambda v: sorted(v)[len(v) // 2] if len(v) % 2 else (sorted(v)[len(v) // 2 - 1] + sorted(v)[len(v) // 2]) / 2
    hit = lambda r: med([d["hittersearly"] for d in ED[r]]) if len(ED.get(r, [])) >= 2 else None
    ral = lambda r: sum(d["rallied"] for d in ED.get(r, [])) / scale(r)
    print(f"  allies on an enemy down (first 1.5 s) AUC {auc(hit):4.2f}  won {q([hit(r) for r in won if hit(r) is not None])}  lost {q([hit(r) for r in lost if hit(r) is not None])}")
    print(f"  enemy downs rallied, per 10 allies per minute: AUC {auc(ral):4.2f}  won {q([ral(r) for r in won])}  lost {q([ral(r) for r in lost])}")
    share = lambda rs, a, b: q([S[r][a] / S[r][b] for r in rs if S[r][b] >= 2])
    print(f"  share of ally downs that died, per round: won {share(won, 'deaths', 'downs')}, lost {share(lost, 'deaths', 'downs')}")
    print(f"  share of enemy downs that died, per round: won {share(won, 'edeaths', 'edowns')}, lost {share(lost, 'edeaths', 'edowns')}")


if __name__ == "__main__":
    want = set(sys.argv[1:]) or {"ally", "checks", "enemy", "revives", "refs"}
    rounds = {r["round"]: r for r in load("rounds.csv")}
    players = {(p["round"], p["p"]): p for p in load("players.csv")}
    real = real_rounds(rounds)
    risk = None
    if "ally" in want or "checks" in want:
        risk = section_ally(rounds, players, real)
    if "checks" in want:
        section_checks(rounds, players, real, risk)
    if "enemy" in want:
        section_enemy(real)
    if "revives" in want:
        section_revives(real)
    if "refs" in want:
        section_refs(rounds)
