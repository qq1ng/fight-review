"""Convert Elite Insights' instant-cast rules into tools/cast_rules.json. Standard library only.

Instant skills (no animation) have no cast event in the log. Elite Insights (MIT) finds them with about 600
per-skill rules in its profession helpers: "the caster gained buff X", "effect Y played on the caster",
"damage from skill Z", "a minion of species S spawned"... This script reads those C# files, copied into
reference/ei/, and writes the rules as data that tools/instant_casts.py applies to a log.

Rules with custom check code (a C# lambda) can't be converted; they are kept but flagged "custom", and
tools/instant_casts.py skips them unless a skill has no other rule.

Usage:
    python tools/gen_cast_rules.py        (re-run after refreshing reference/ei from Elite Insights)
"""
import glob
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
EI = os.path.join(HERE, "..", "reference", "ei")
OUT = os.path.join(HERE, "cast_rules.json")


def read(path):
    with open(path, encoding="utf-8-sig") as f:
        return f.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def constants():
    c = {}
    for path in glob.glob(os.path.join(EI, "**", "*.cs"), recursive=True):
        text = strip_comments(read(path))
        for name, value in re.findall(r"const (?:long|int|ulong) (\w+) = (-?\d+)", text):
            c.setdefault(name, int(value))
        for name, value in re.findall(r"static readonly Guid (\w+) = new\(\"([0-9A-Fa-f]{32})\"\)", text):
            c.setdefault("EffectGUIDs." + name, value.upper())
    species = strip_comments(read(os.path.join(EI, "ParserHelpers", "IDs", "SpeciesIDs.cs")))
    for enum, body in re.findall(r"enum (\w+) : int\s*\{(.*?)\}", species, flags=re.S):
        for name, value in re.findall(r"(\w+)\s*=\s*(\d+)", body):
            c.setdefault(f"{enum}.{name}", int(value))
    builds = strip_comments(read(os.path.join(EI, "ParserHelpers", "GW2Builds.cs")))
    for name, value in re.findall(r"const ulong (\w+) = (\d+)", builds):
        c["GW2Builds." + name] = int(value)
    c["GW2Builds.StartOfLife"] = 0
    c["GW2Builds.EndOfLife"] = 2 ** 63
    return c


def balanced(text, i, open_="(", close=")"):
    """Index just past the bracket matching text[i]."""
    depth = 0
    for j in range(i, len(text)):
        if text[j] in "([{":
            depth += 1
        elif text[j] in ")]}":
            depth -= 1
            if depth == 0:
                return j + 1
    raise ValueError("unbalanced")


def split_top(text, sep=","):
    parts, depth, cur = [], 0, []
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return parts


def rule_lists(text):
    for m in re.finditer(r"List<InstantCastFinder>\s+\w+\s*=\s*(?:new\(\)\s*)?\[", text):
        start = m.end() - 1
        yield text[start + 1:balanced(text, start, "[", "]") - 1]


def value(arg, c):
    arg = arg.strip()
    arg = re.sub(r"^\((?:int|long)\)", "", arg)
    if arg.startswith("-") and not re.fullmatch(r"-\d+", arg):
        return -value(arg[1:], c)
    if re.fullmatch(r"-?\d+", arg):
        return int(arg)
    if arg.startswith("["):
        return [value(a, c) for a in split_top(arg[1:-1])]
    if arg in c:
        return c[arg]
    if "." in arg and arg.split(".", 1)[1] in c:
        return c[arg.split(".", 1)[1]]
    raise KeyError(arg)


SPEC_CHECK = re.compile(r"Using(Src|Dst|By|To)(Base)?(Not)?(Spec|Specs)Checker")


def parse_rule(item, c, source):
    m = re.match(r"new (\w+)\(", item)
    if not m:
        return None
    end = balanced(item, m.end() - 1)
    args = split_top(item[m.end():end - 1])
    rule = {"type": m.group(1), "source": source, "icd": 50, "builds": [0, 2 ** 63], "checks": [],
            "custom": False, "minions": False, "time_offset": 0}
    try:
        rule["skill"] = value(args[0], c)
        rule["arg"] = value(args[1], c) if len(args) > 1 else None
    except KeyError as e:
        rule["unresolved"] = str(e)
        return rule
    rest = item[end:]
    for mod in re.finditer(r"\.(\w+)\(", rest):
        name = mod.group(1)
        margs = rest[mod.end():balanced(rest, mod.end() - 1) - 1]
        a = split_top(margs)
        try:
            if name == "WithBuilds":
                rule["builds"] = [value(a[0], c), value(a[1], c) if len(a) > 1 else 2 ** 63]
            elif name == "UsingICD":
                rule["icd"] = value(a[0], c) if a else 50
            elif name == "UsingTimeOffset":
                rule["time_offset"] = value(a[0], c)
            elif name == "WithMinions":
                rule["minions"] = True
            elif name in ("UsingDisableWithEffectData", "UsingDisableWithMissileData"):
                rule["disabled_with"] = "effect" if "Effect" in name else "missile"
            elif name == "UsingDurationChecker":
                rule["checks"].append({"kind": "duration", "values": [value(x, c) for x in a if "=" not in x]})
            elif name in ("UsingIsAroundDstChecker", "UsingNotIsAroundDstChecker"):
                rule["checks"].append({"kind": "around_dst", "want": name == "UsingIsAroundDstChecker"})
            elif name == "UsingSecondaryEffectSameSrcChecker":
                rule["checks"].append({"kind": "secondary_effect", "guid": value(a[0], c),
                                       "offset": value(a[1], c) if len(a) > 1 else 0})
            elif SPEC_CHECK.fullmatch(name):
                who, base, neg, _ = SPEC_CHECK.fullmatch(name).groups()
                specs = re.findall(r"Spec\.(\w+)", margs)
                rule["checks"].append({"kind": "spec", "who": who.lower(), "base": bool(base),
                                       "negate": bool(neg), "specs": specs})
            elif name in ("UsingChecker", "UsingSecondaryEffectInvertedSrcChecker", "UsingEnable",
                          "UsingNoSecondaryEffectSameSrcCheckerOnSamePosition", "UsingActorCheckerByPresence",
                          "UsingActorCheckerByAbsence", "UsingNoAnimatedCastChecker"):
                rule["custom"] = True
                rule.setdefault("custom_code", []).append(f".{name}({' '.join(margs.split())[:160]})")
            # UsingOrigin, UsingNotAccurate, UsingBefore/AfterWeaponSwap, WithEvtcBuilds...: display or
            # small time adjustments only
        except KeyError as e:
            rule["unresolved"] = str(e)
    return rule


CPP_OUT = os.path.join(HERE, "..", "src", "CastRules.inc")
CPP_TYPES = ["BuffGainCastFinder", "BuffGiveCastFinder", "BuffLossCastFinder", "MinionCommandCastFinder",
             "DamageCastFinder", "EXTHealingCastFinder", "EXTBarrierCastFinder", "EffectCastFinder",
             "EffectCastFinderByDst", "MissileCastFinder", "MinionCastCastFinder", "MinionSpawnCastFinder"]


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_cpp(rules, names, fallback, minion_command_buff):
    """The same rules as a C++ table for src/InstantCasts.cpp: only the ones it can apply."""
    lines = ["// Generated by tools/gen_cast_rules.py from Elite Insights (MIT). Do not edit.",
             f"constexpr uint32_t kMinionCommandBuff = {minion_command_buff};", "const CastRule kCastRules[] = {"]
    for r in rules:
        if r["custom"] or "unresolved" in r or r["type"] not in CPP_TYPES:
            continue
        arg = r["arg"]
        species = arg if isinstance(arg, list) else [arg] if r["type"] == "MinionSpawnCastFinder" else []
        guid = arg if isinstance(arg, str) else ""
        arg_id = arg if isinstance(arg, int) else 0
        spec = [c for c in r["checks"] if c["kind"] == "spec"]
        dur = [c for c in r["checks"] if c["kind"] == "duration"]
        around = [c for c in r["checks"] if c["kind"] == "around_dst"]
        second = [c for c in r["checks"] if c["kind"] == "secondary_effect"]
        spec_text = ";".join(f"{c['who']}{'!' if c['negate'] else ''}{'^' if c['base'] else ''}={'|'.join(c['specs'])}"
                             for c in spec)
        dmin = dur[0]["values"][0] if dur else -1
        dmax = dur[0]["values"][-1] if dur else -1
        kind = r["type"].replace("CastFinder", "")
        species_text = ", ".join(str(s) for s in species[:4]) or "0"
        disabled = {"effect": 1, "missile": 2}.get(r.get("disabled_with"), 0)
        around_code = (1 if around[0]["want"] else 2) if around else 0
        second_guid = cstr(second[0]["guid"]) if second else cstr("")
        second_offset = second[0]["offset"] if second else 0
        lines.append(
            f"\t{{CR_{kind}, {r['skill']}, {arg_id}, {cstr(guid)}, {{{species_text}}}, {r['icd']}, "
            f"{r['builds'][0]}ULL, {min(r['builds'][1], 2 ** 64 - 1)}ULL, {str(r['minions']).lower()}, "
            f"{r['time_offset']}, {disabled}, {cstr(spec_text)}, {dmin}, {dmax}, {around_code}, {second_guid}, "
            f"{second_offset}}},")
    lines.append("};")
    lines.append("const std::pair<int32_t, const char*> kSkillNames[] = {")
    for sid, name in sorted(names.items()):
        lines.append(f"\t{{{sid}, {cstr(name)}}},")
    lines.append("};")
    lines.append("const std::pair<int32_t, const char*> kFallbackSkillNames[] = {")
    for sid, name in sorted(fallback.items()):
        lines.append(f"\t{{{sid}, {cstr(name)}}},")
    lines.append("};")
    with open(CPP_OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


ICON_OUT = os.path.join(HERE, "..", "src", "SkillIconData.inc")


def write_icons(c):
    """Icon URLs for skills the GW2 API's /v2/skills lacks (relics, boons, traits, combos): Elite Insights'
    explicit id -> image table, then its image constants whose name matches a skill id constant, and every image
    by a name key (letters and digits, lowercase) so a skill can be found by its name in the log."""
    images = {}
    for path in glob.glob(os.path.join(EI, "ParserHelpers", "Images", "*.cs")):
        # raw text: stripping "//" comments would cut every https:// address
        for name, url in re.findall(r'const string (\w+) = "(https://[^"]+\.png)"', read(path)):
            images.setdefault(name, url)
    by_id = {}
    overrides = strip_comments(read(os.path.join(EI, "ParsedData", "Skills", "SkillItemOverrides.cs")))
    block = overrides[overrides.find("OverridenSkillIcons"):]  # identifiers only here, no URLs
    for const, image in re.findall(r"\{\s*(\w+),\s*\w+Images\.(\w+)\s*\}", block):
        if const in c and image in images:
            by_id.setdefault(c[const], images[image])
    for name, url in images.items():
        if name in c and isinstance(c[name], int):
            by_id.setdefault(c[name], url)
    by_name = {}
    for name, url in images.items():
        by_name.setdefault(name.lower(), url)
    lines = ["// Generated by tools/gen_cast_rules.py from Elite Insights (MIT). Do not edit.",
             "const std::pair<int32_t, const char*> kIconById[] = {"]
    lines += [f"\t{{{i}, {cstr(u)}}}," for i, u in sorted(by_id.items())]
    lines += ["};", "const std::pair<const char*, const char*> kIconByName[] = {"]
    lines += [f"\t{{{cstr(n)}, {cstr(u)}}}," for n, u in sorted(by_name.items())]
    lines += ["};"]
    with open(ICON_OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"icons: {len(by_id)} by id, {len(by_name)} by name")


def main():
    c = constants()
    write_icons(c)
    rules = []
    for path in sorted(glob.glob(os.path.join(EI, "EIData", "ProfHelpers", "**", "*.cs"), recursive=True)):
        text = strip_comments(read(path))
        source = os.path.splitext(os.path.basename(path))[0]
        for body in rule_lists(text):
            for item in split_top(body):
                rule = parse_rule(item, c, source)
                if rule:
                    rules.append(rule)
    # Names Elite Insights shows for skills the log names differently or not at all (its own negative ids
    # for merged skills like "Blink or Phase Retreat")
    names = {}
    overrides = strip_comments(read(os.path.join(EI, "ParsedData", "Skills", "SkillItemOverrides.cs")))
    for const, name in re.findall(r"\{\s*(\w+),\s*\"([^\"]+)\"\s*\}", overrides):
        if const in c:
            names[c[const]] = name
    # Fallback for rule skills the log's skill table may lack: Elite Insights' constant name, spaced out.
    # Use only after the log's own names.
    fallback = {}
    by_id = {}
    for const, v in c.items():
        if isinstance(v, int) and "." not in const:
            by_id.setdefault(v, const)
    for r in rules:
        sid = r.get("skill")
        if isinstance(sid, int) and sid not in names and sid in by_id:
            fallback[sid] = re.sub(r"(?<=[a-z])(?=[A-Z0-9])", " ", by_id[sid])
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump({"source": "Elite Insights (MIT), github.com/baaron4/GW2-Elite-Insights-Parser, "
                             "via reference/ei; generated by tools/gen_cast_rules.py",
                   "minion_command_buff": c["MinionCommandBuff"], "names": names,
                   "fallback_names": fallback, "rules": rules}, f, indent=0)
    write_cpp(rules, names, fallback, c["MinionCommandBuff"])
    types = {}
    for r in rules:
        types[r["type"]] = types.get(r["type"], 0) + 1
    print(f"{len(rules)} rules: {types}")
    print(f"custom: {sum(r['custom'] for r in rules)}, unresolved: {sum('unresolved' in r for r in rules)}, "
          f"skill names: {len(names)} + {len(fallback)} fallback")
    for r in rules:
        if "unresolved" in r:
            print("  unresolved", r["source"], r["type"], r["unresolved"])


if __name__ == "__main__":
    main()
