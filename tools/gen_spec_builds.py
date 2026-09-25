"""Main jobs per spec over every log on the machine.

For each player-round (30 s+ alive): each job per second or minute alive, against that round's squad average (so a
busy fight and a quiet one compare). A player-round's group = the job it did most against the squad (x1.5 at
least), else "flexible". Per spec: how player-rounds split, and each group's top jobs. The groups are not separate
builds (the user, 2026-09-24: players on a spec mostly run the same build; the spread is how they play it), so the
output merges them into one job list per spec. src/SpecJobs.inc was first written from this; since the user's pass
(2026-09-24) it is kept by hand and this script only prints the audit. Needs build/release/frcheck.exe; its output is cached in
build/frcache. No player names in the output.

Usage:
    python tools/gen_spec_builds.py [log folder]   (default: Documents/Guild Wars 2/addons/arcdps/arcdps.cbtlogs)
"""
import collections, concurrent.futures, glob, hashlib, os, statistics, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
FR = os.path.join(ROOT, "build", "release", "frcheck.exe")
CACHE = os.path.join(ROOT, "build", "frcache")
os.makedirs(CACHE, exist_ok=True)
BOONS = ["Might", "Fury", "Quickness", "Alacrity", "Protection", "Regeneration", "Vigor", "Aegis", "Stability",
         "Swiftness", "Resistance", "Resolution"]
NAMES = {"damage": "Damage to players /s", "strips": "Strips /min", "cleanses": "Cleanses /min",
         "healing": "Healing /s", "barrier": "Barrier /s"}
NAMES.update({b: b + " on subgroup" for b in BOONS})
BUILD = {"damage": "damage", "strips": "strip", "cleanses": "cleanse", "healing": "heal", "barrier": "barrier"}
BUILD.update({b: b.lower() for b in BOONS})

FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.expanduser("~"), "Documents", "Guild Wars 2", "addons", "arcdps", "arcdps.cbtlogs")
logs = sorted(glob.glob(os.path.join(FOLDER, "**", "*.zevtc"), recursive=True))


def run(path):
    key = os.path.join(CACHE, hashlib.md5(path.encode()).hexdigest() + ".txt")
    if os.path.exists(key):
        return open(key, encoding="utf-8").read()
    out = subprocess.run([FR, path], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    open(key, "w", encoding="utf-8").write(out)
    return out


with concurrent.futures.ThreadPoolExecutor(8) as pool:
    outputs = list(pool.map(run, logs))

rows = collections.defaultdict(list)  # spec -> [ratios]
rounds = 0
for out in outputs:
    players = []
    for line in out.splitlines():
        if not line.startswith("player\t"):
            continue
        f = line.split("\t")
        active = float(f[9])
        if active < 30000:
            continue
        sec, mins = active / 1000, active / 60000
        job = {"damage": float(f[7]) / sec, "strips": float(f[10]) / mins, "cleanses": float(f[11]) / mins}
        if f[4] == "1":
            job["healing"] = float(f[5]) / sec
            job["barrier"] = float(f[6]) / sec
        for i, b in enumerate(BOONS):
            job[b] = float(f[16 + i])
        players.append((f[2], job))
    if len(players) < 5:
        continue
    rounds += 1
    mean = {k: statistics.mean([j[k] for s, j in players if k in j] or [0]) for k in NAMES}
    for spec, job in players:
        rows[spec].append({k: job[k] / mean[k] for k in job if mean.get(k, 0) > 0})

print(f"{len(logs)} logs, {rounds} rounds with 5+ players, {sum(len(r) for r in rows.values())} player-rounds, {len(rows)} specs\n")
builds_out = {}
for spec in sorted(rows, key=lambda s: -len(rows[s])):
    labels = collections.Counter()
    by = collections.defaultdict(list)
    for r in rows[spec]:
        top = max(r, key=r.get) if r else None
        label = top if top and r[top] >= 1.5 else "flexible"
        labels[label] += 1
        by[label].append(r)
    n = len(rows[spec])
    parts = []
    builds = []
    for label, c in labels.most_common():
        share = c / n
        med = {k: statistics.median([r.get(k, 0) for r in by[label]]) for k in NAMES}
        jobs = [k for k in sorted(med, key=lambda k: -med[k]) if med[k] >= 1.5][:3]
        parts.append(f"{BUILD.get(label, label)} {share:.0%}" + (" (" + ", ".join(f"{j} x{med[j]:.1f}" for j in jobs) + ")" if jobs else ""))
        if label != "flexible" and share >= 0.15 and c >= 10 and jobs:
            builds.append((BUILD[label], label, share, jobs))
    print(f"{spec:14s} {n:5d}  " + "; ".join(parts[:4]))
    if builds:
        builds_out[spec] = builds

# src/SpecJobs.inc is kept by hand since the user's pass (2026-09-24, notes/SPEC_JOBS_PASS.md, local): this script only prints
# the audit above to check it against.
