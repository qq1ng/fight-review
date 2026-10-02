"""The C++ cast counts (frcheck) against Elite Insights' Skill Usage in a TopStats WvW_Combat_Summary report, by skill
(names compared without punctuation or a trailing " Skill"). Prints per skill: ours, theirs, rows that differ. No names.
Usage: python tools/check_casts_ei.py <summary.json.gz> <log folder>"""
import collections, os, re, subprocess, sys
norm = lambda n: re.sub(r' skill$', '', re.sub(r'[^a-z0-9 ]', '', n.lower())).strip()
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare_summary as cs
from evtc import Log, spec_name
from skills import SkillUsage

summary, folder = sys.argv[1], sys.argv[2]
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "release", "frcheck.exe")
pages = cs.load(summary)
ours = collections.defaultdict(collections.Counter)  # (account, spec) -> name -> casts
for n, when, pov in cs.fights(pages):
    path = cs.log_near(when, folder)
    if not path:
        continue
    log = Log(path)
    su = SkillUsage(log)
    spec = {a["account"]: spec_name(a) for a in log.agents.values() if a["player"] and a["account"]}
    out = subprocess.run([EXE, path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        c = line.split("\t")
        if c[0] != "skill" or len(c) < 4:
            continue
        acc, sid, casts = c[1], int(c[2]), int(c[3])
        if acc in spec and casts:
            ours[(acc, spec[acc])][norm(su.name(sid))] += casts
usage = {k: (ft, collections.Counter({norm(n): c for n, c in ts.items()})) for k, (ft, ts) in cs.skill_usage(pages).items()}
by_skill = collections.defaultdict(lambda: [0, 0, 0, 0, set()])  # name -> ours, theirs, rows, rows off, specs
rows = exact = 0
for acc, (ft, ts) in usage.items():
    names = set(ts) | set(ours[acc])
    for name in names:
        t, o = round(ts.get(name, 0)), ours[acc].get(name, 0)
        if t == 0 and o == 0:
            continue
        rows += 1
        exact += t == o
        b = by_skill[name]
        b[0] += o; b[1] += t; b[2] += 1; b[3] += abs(o - t) > max(1, 0.1 * t); b[4].add(acc[1])
print(f"{rows} player-skill rows, {exact} exact ({100 * exact // max(rows, 1)}%)")
print(f"\n{'skill':32} {'ours':>6} {'EI':>6} {'rows':>5} {'off':>4}  specs")
for name, (o, t, r, off, specs) in sorted(by_skill.items(), key=lambda kv: -abs(kv[1][0] - kv[1][1])):
    if abs(o - t) < 5:
        continue
    print(f"{name[:32]:32} {o:6} {t:6} {r:5} {off:4}  {','.join(sorted(specs))[:60]}")
