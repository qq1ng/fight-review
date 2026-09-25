"""Compare the C++ analysis (build/release/frcheck.exe) with the Python prototype on some logs.

Cast counts differ by design (the C++ counts learned instant casts, notes/HANDOFF.md), so they are compared only
with --casts.

Usage:
    python tools/check_cpp.py [--casts] <file.zevtc> [more ...]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import Log, SC, STATECHANGE, SRC, spec_name
from fight_stats import FightStats
from skills import SkillUsage
from boons import BoonGeneration

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "..", "build", "release", "frcheck.exe")
BOON_ORDER = [740, 725, 1187, 30328, 717, 718, 726, 743, 1122, 719, 26980, 873]


def main(paths, casts=False):
    bad = 0
    for path in paths:
        out = subprocess.run([EXE, path], capture_output=True, text=True, encoding="utf-8", errors="replace")
        if out.returncode:
            print(os.path.basename(path), "frcheck failed:", out.stderr.strip())
            bad += 1
            continue
        cpp = {}
        for line in out.stdout.splitlines():
            f = line.split("\t")
            if f[0] == "player":
                cpp[f[1]] = f
        log = Log(path)
        fs, su, bg = FightStats(log), SkillUsage(log), BoonGeneration(log)
        diffs = []
        for a in su.players:
            acc = log.agents[a]["account"]
            c = cpp.get(acc)
            if not c:
                diffs.append(f"{acc}: missing in C++")
                continue
            p = fs.p[a]
            py = {"heal": p["heal"], "barrier": p["barrier"], "damage": sum(su.damage_players[a].values()),
                  "damage_all": sum(su.damage[a].values()), "active": su.active_ms[a], "strips": p["strips"],
                  "cleanses": p["cleanses"], "evades": p["evades"], "blocks": p["blocks"], "invulns": p["invulns"]}
            cc = {"heal": int(c[5]), "barrier": int(c[6]), "damage": int(c[7]), "damage_all": int(c[8]),
                  "active": int(c[9]), "strips": int(c[10]), "cleanses": int(c[11]), "evades": int(c[12]),
                  "blocks": int(c[13]), "invulns": int(c[14])}
            if casts:
                py["casts"] = sum(su.casts[a].values())
                cc["casts"] = int(c[-1])
            for k in py:
                if py[k] != cc[k]:
                    diffs.append(f"{acc} {k}: py {py[k]} c++ {cc[k]}")
            for i, b in enumerate(BOON_ORDER):
                pv, cv = bg.squad_generation(a, b), float(c[16 + i])
                if abs(pv - cv) > max(0.002, 0.005 * abs(pv)):
                    diffs.append(f"{acc} boon {b}: py {pv:.4f} c++ {cv:.4f}")
        extra = set(cpp) - {log.agents[a]["account"] for a in su.players}
        if extra:
            diffs.append(f"only in C++: {sorted(extra)}")
        print(f"{os.path.basename(path)}: {len(su.players)} players, {len(diffs)} differences")
        for d in diffs[:12]:
            print("   ", d)
        bad += bool(diffs)
    return bad


if __name__ == "__main__":
    args = sys.argv[1:]
    sys.exit(main([a for a in args if a != "--casts"], "--casts" in args))
