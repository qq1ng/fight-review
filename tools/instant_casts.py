"""Instant casts: skills without an animation, found from what they leave in the log. Standard library only.

Applies Elite Insights' rules (tools/cast_rules.json, made by tools/gen_cast_rules.py) the way Elite
Insights does: each rule watches one kind of event (a buff gained, lost or given, an effect, a hit, a heal,
a missile, a minion spawning or casting), and every match by the same caster counts as one cast unless it
follows the previous match within the rule's ICD (50 ms by default).

Not applied: rules with custom C# check code, EngineerKitFinder, BandTogetherCastFinder and breakbar damage.

Usage:
    python tools/instant_casts.py <file.zevtc> [account]
"""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evtc import (Log, SC, TIME, SRC, DST, VALUE, BUFF_DMG, SKILL, SRC_INST, SRC_MASTER, DST_MASTER, IFF, BUFF,
                  RESULT, ACTIVATION, SHIELDS, OFFCYCLE, STATECHANGE, PROFESSIONS, spec_name)

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_DELAY = 10  # Elite Insights' ServerDelayConstant, ms
HEAL_SELF_REPORTED = 128

with open(os.path.join(HERE, "cast_rules.json"), encoding="utf-8") as f:
    _TABLE = json.load(f)
RULES = _TABLE["rules"]
NAMES = {int(k): v for k, v in _TABLE["names"].items()}  # Elite Insights' display names: use first
FALLBACK_NAMES = {int(k): v for k, v in _TABLE["fallback_names"].items()}  # use after the log's names
MINION_COMMAND_BUFF = _TABLE["minion_command_buff"]


class _Context:
    """The parts of a log the rules look at, indexed once."""

    def __init__(self, log):
        self.log = log
        ev = log.events
        self.build = next((e[SRC] for e in ev if e[STATECHANGE] == SC["GWBUILD"]), 0)
        guids = {e[SKILL]: struct.pack("<QQ", e[SRC], e[DST]).hex().upper()
                 for e in ev if e[STATECHANGE] == SC["IDTOGUID"]}

        # Minions: master from the master instance id their events carry
        inst_to_player = {}
        for e in ev:
            if e[STATECHANGE] == 0 and log.agents.get(e[SRC], {}).get("player"):
                inst_to_player.setdefault(e[SRC_INST], e[SRC])
        self.master = {}
        for e in ev:
            if e[SRC_MASTER] and e[SRC] not in self.master and not log.agents.get(e[SRC], {}).get("player"):
                if e[SRC_MASTER] in inst_to_player:
                    self.master[e[SRC]] = inst_to_player[e[SRC_MASTER]]
            if e[DST_MASTER] and e[DST] not in self.master and not log.agents.get(e[DST], {}).get("player"):
                if e[DST_MASTER] in inst_to_player:
                    self.master[e[DST]] = inst_to_player[e[DST_MASTER]]

        self.effects = collections.defaultdict(list)  # guid -> [(time, src, dst or None, duration)]
        agent_fx, ground_fx = SC["EFFECTAGENTCREATE"], SC["EFFECTGROUNDCREATE"]
        self.buff_apply = collections.defaultdict(list)  # buff -> [(time, by, to, duration)]
        self.buff_loss = collections.defaultdict(list)  # buff -> [(time, to)]
        self.damage = collections.defaultdict(list)  # skill -> [(time, from)]
        self.heals = collections.defaultdict(list)  # skill -> [(time, from, barrier)]
        self.missiles = collections.defaultdict(list)  # skill -> [(time, src)]
        self.minion_casts = collections.defaultdict(list)  # skill -> [(time, minion)]
        self.spawns = collections.defaultdict(list)  # agent -> [time]
        first_seen = {}
        for e in ev:
            sc, t = e[STATECHANGE], e[TIME]
            if sc == 0:
                self.damage[e[SKILL]].append((t, e[SRC]))
                first_seen.setdefault(e[SRC], t)
            elif sc in (agent_fx, ground_fx):
                guid = guids.get(e[SKILL])
                if guid:
                    duration = e[IFF] | e[BUFF] << 8 | e[RESULT] << 16 | e[ACTIVATION] << 24
                    self.effects[guid].append((t, e[SRC], e[DST] if sc == agent_fx else None, duration))
            elif sc == SC["BUFFAPPLY"]:
                self.buff_apply[e[SKILL]].append((t, e[SRC], e[DST], e[VALUE]))
            elif sc == SC["BUFFREMOVE_ALL"]:
                self.buff_loss[e[SKILL]].append((t, e[SRC]))
            elif sc == SC["EXTENSIONCOMBAT"] and e[OFFCYCLE] & HEAL_SELF_REPORTED:
                self.heals[e[SKILL]].append((t, e[SRC], bool(e[SHIELDS])))
            elif sc == SC["MISSILECREATE"]:
                self.missiles[e[SKILL]].append((t, e[SRC]))
            elif sc == SC["ANIMATIONSTART"] and e[SRC] in self.master:
                self.minion_casts[e[SKILL]].append((t, e[SRC]))
            elif sc == SC["SPAWN"]:
                self.spawns[e[SRC]].append(t)
        for a in self.master:
            if a not in self.spawns and a in first_seen:
                self.spawns[a].append(first_seen[a])
        self.has_effects = bool(self.effects)
        self.has_missiles = bool(self.missiles)

    def final_master(self, a):
        return self.master.get(a, a)

    def spec(self, a, base):
        ag = self.log.agents.get(a)
        if not ag or not ag["player"]:
            return None
        return PROFESSIONS.get(ag["prof"]) if base else spec_name(ag)

    def species(self, a):
        ag = self.log.agents.get(a)
        if not ag or ag["player"] or ag["prof"] >> 16 == 0xFFFF:
            return None
        return ag["prof"] & 0xFFFF


def _matches(rule, ctx):
    """Yield (time, caster) for every event the rule accepts, before the ICD."""
    kind, arg, minions = rule["type"], rule["arg"], rule["minions"]
    checks = rule["checks"]

    def spec_ok(who_agents):
        for c in checks:
            if c["kind"] != "spec":
                continue
            agent = who_agents.get(c["who"])
            spec = ctx.spec(agent, c["base"]) if agent is not None else None
            if (spec in c["specs"]) == c["negate"]:
                return False
        return True

    if kind in ("BuffGainCastFinder", "MinionCommandCastFinder", "BuffGiveCastFinder"):
        buff = MINION_COMMAND_BUFF if kind == "MinionCommandCastFinder" else arg
        for t, by, to, duration in ctx.buff_apply.get(buff, ()):
            if kind == "MinionCommandCastFinder" and (ctx.species(to) != arg or to not in ctx.master):
                continue
            durations = [c for c in checks if c["kind"] == "duration"]
            if durations and not any(abs(duration - c["values"][0]) < SERVER_DELAY for c in durations):
                continue
            if not spec_ok({"by": by, "to": to, "src": by, "dst": to}):
                continue
            key = by if kind == "BuffGiveCastFinder" else to
            yield t, ctx.final_master(key) if minions or kind != "BuffGainCastFinder" else key
    elif kind == "BuffLossCastFinder":
        for t, to in ctx.buff_loss.get(arg, ()):
            if spec_ok({"to": to, "dst": to}):
                yield t, ctx.final_master(to) if minions else to
    elif kind == "DamageCastFinder":
        for t, src in ctx.damage.get(arg, ()):
            yield t, src
    elif kind in ("EXTHealingCastFinder", "EXTBarrierCastFinder"):
        barrier = kind == "EXTBarrierCastFinder"
        for t, src, is_barrier in ctx.heals.get(arg, ()):
            if is_barrier == barrier:
                yield t, src
    elif kind in ("EffectCastFinder", "EffectCastFinderByDst"):
        by_dst = kind == "EffectCastFinderByDst"
        events = ctx.effects.get(arg, ())
        for t, src, dst, duration in events:
            key = dst if by_dst else src
            if key is None:
                continue
            ok = spec_ok({"src": src, "dst": dst})
            for c in checks:
                if not ok:
                    break
                if c["kind"] == "around_dst":
                    ok = (dst is not None) == c["want"]
                elif c["kind"] == "duration":
                    v = c["values"]
                    ok = duration == v[0] if len(v) == 1 else v[0] <= duration <= v[1]
                elif c["kind"] == "secondary_effect":
                    other = ctx.effects.get(c["guid"], ())
                    # a different effect (other GUID) by the same agent at the same moment; Elite Insights only
                    # excludes the event itself, which can't be in another GUID's list
                    ok = any((d if by_dst else s) == key and abs(t2 - c["offset"] - t) < SERVER_DELAY
                             for t2, s, d, _ in other)
            if ok:
                yield t, ctx.final_master(key) if minions else key
    elif kind == "MissileCastFinder":
        for t, src in ctx.missiles.get(arg, ()):
            yield t, ctx.final_master(src) if minions else src
    elif kind == "MinionCastCastFinder":
        for t, minion in ctx.minion_casts.get(arg, ()):
            yield t, ctx.final_master(minion)
    elif kind == "MinionSpawnCastFinder":
        wanted = set(arg) if isinstance(arg, list) else {arg}
        for minion, master in ctx.master.items():
            if ctx.species(minion) in wanted:
                for t in ctx.spawns.get(minion, ()):
                    yield t, master


SKIPPED_TYPES = {"EngineerKitFinder", "BandTogetherCastFinder", "BreakbarDamageCastFinder"}


def instant_casts(log):
    """{caster addr: {skill id: [times]}} for every player in the log."""
    ctx = _Context(log)
    out = collections.defaultdict(lambda: collections.defaultdict(list))
    for rule in RULES:
        if rule["custom"] or "unresolved" in rule or rule["type"] in SKIPPED_TYPES:
            continue
        if not rule["builds"][0] <= ctx.build < rule["builds"][1]:
            continue
        if rule.get("disabled_with") == "effect" and ctx.has_effects:
            continue
        if rule.get("disabled_with") == "missile" and ctx.has_missiles:
            continue
        last = collections.defaultdict(lambda: -10 ** 12)
        for t, caster in sorted(_matches(rule, ctx), key=lambda m: m[0]):
            if caster is None or not log.agents.get(caster, {}).get("player"):
                continue
            if t - last[caster] < rule["icd"]:
                last[caster] = t
                continue
            last[caster] = t
            out[caster][rule["skill"]].append(t + rule["time_offset"])
    return out


def main(path, account=None):
    log = Log(path)
    found = instant_casts(log)
    for caster, skills in found.items():
        ag = log.agents[caster]
        if account and ag["account"] != account:
            continue
        if not ag["account"]:
            continue
        line = ", ".join(f"{NAMES.get(s) or log.skills.get(s) or FALLBACK_NAMES.get(s, s)} {len(ts)}" for s, ts in
                         sorted(skills.items(), key=lambda kv: -len(kv[1])))
        print(f"{ag['name'][:24]:24} {spec_name(ag):13} {line}")


if __name__ == "__main__":
    main(*sys.argv[1:])
