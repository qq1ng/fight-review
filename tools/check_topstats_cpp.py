"""Check the C++ analysis (build/release/frcheck.exe) directly against a TopStats report, player by player.

tools/compare_topstats.py checks the Python prototype, and tools/check_cpp.py checks the C++ against the
prototype. This one covers what only the C++ has: downs, deaths, CC taken and time reviving
(the down, revive and stability views rest on them), plus the shared counters as a cross-check.

Fights are matched to logs by end time, to the minute; players by account. TopStats writes -1 for "not in
this fight". It sometimes read another player's log for a fight; those fights are counted apart, since the
squad and what each client saw differ.

Usage:
    python tools/check_topstats_cpp.py <report.json.gz> <log folder> [--detail KEY]
"""
import collections
import gzip
import json
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "..", "build", "release", "frcheck.exe")

# TopStats key -> (frcheck line, field index); "player" fields: 5 heal ... 14 invulns
KEYS = {
    "cleanses": ("player", 11), "rips": ("player", 10), "evades": ("player", 12), "blocks": ("player", 13),
    "invulns": ("player", 14), "heal": ("player", 5),
    "downed": ("events", 2), "deaths": ("events", 3), "receivedCrowdControl": ("events", 4),
    "resOutTime": ("events", 6), "appliedCrowdControl": ("events", 7), "downContribution": ("events", 9),
    "dodges": ("events", 10),
}
# Not compared: dmg_taken. TopStats stores it per second of active time (whole seconds), so it can't be turned
# back into a total closely enough; ours came out about 1.5% lower with a wide spread (2026-09-24).


def log_for_fight(fight, folder):
    stamp = fight["end_time"][:16].replace("-", "").replace(" ", "-").replace(":", "")
    names = sorted(n for n in os.listdir(folder) if n.startswith(stamp) and n.endswith(".zevtc"))
    return os.path.join(folder, names[0]) if names else None


def run(path):
    out = subprocess.run([EXE, path], capture_output=True, text=True, encoding="utf-8", errors="replace")
    lines = collections.defaultdict(dict)
    squad = 0
    for line in out.stdout.splitlines():
        f = line.split("\t")
        if f[0] == "fight":
            squad = int(f[2])
        elif f[0] in ("player", "events"):
            lines[f[0]][f[1]] = f
    return squad, lines


def main(report_path, folder, detail=None):
    report = json.loads(gzip.decompress(open(report_path, "rb").read()))
    pairs = collections.defaultdict(list)
    fights = same = 0
    for i, fight in enumerate(report["fights"]):
        path = log_for_fight(fight, folder)
        if not path:
            continue
        fights += 1
        squad, lines = run(path)
        same_squad = fight["squad"] == squad
        same += same_squad
        for player in report["players"]:
            theirs = player["stats_per_fight"][i]
            if theirs.get("evades", -1) == -1:
                continue
            for key, (kind, idx) in KEYS.items():
                row = lines[kind].get(player["account"])
                if row is None or theirs.get(key, -1) == -1:
                    continue
                if key == "heal" and row[4] == "0":
                    continue  # no Healing Stats data on our side: "unknown"
                ours = float(row[idx]) / (1000.0 if key == "resOutTime" else 1.0)
                pairs[key].append((float(theirs[key]), ours, same_squad, i))

    print(f"{fights} fights with a matching log, {same} with the same squad\n")
    print(f"{'metric':22} {'pairs':>6} {'exact':>6} {'<=5%':>6} {'same squad exact':>17} {'median ours/theirs':>19}")
    for key, rows in pairs.items():
        tol = 1.0 if key == "resOutTime" else 0  # TopStats rounds seconds down
        exact = sum(abs(t - o) <= max(tol, 1e-9) for t, o, *_ in rows)
        close = sum(abs(t - o) <= max(0.05 * abs(t), tol) for t, o, *_ in rows)
        rs = [r for r in rows if r[2]]
        same_exact = sum(abs(t - o) <= max(tol, 1e-9) for t, o, *_ in rs)
        ratios = [o / t for t, o, *_ in rows if t]
        med = statistics.median(ratios) if ratios else float("nan")
        print(f"{key:22} {len(rows):>6} {exact:>6} {close:>6} {same_exact:>8} of {len(rs):<5} {med:>19.3f}")
        if detail == key:
            for t, o, s, i in sorted(rows, key=lambda r: -abs(r[0] - r[1]))[:15]:
                print(f"    fight {i:2} {'same' if s else 'other'}  theirs {t:>10.1f}  ours {o:>10.1f}")


if __name__ == "__main__":
    args = sys.argv[1:]
    detail = args[args.index("--detail") + 1] if "--detail" in args else None
    main(args[0], args[1], detail)
