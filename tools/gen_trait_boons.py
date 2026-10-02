"""Traits that give boons, and what sets them off, from the public GW2 API.

A boon given to an ally that no skill explains ("other sources") often comes from a trait. The log doesn't name the
trait or say which traits a player took, but it shows what they did at that moment: a dodge, a weapon swap, a heal
or elite skill, a skill of a kind (a mantra, a shout, a shatter). This lists, per trait that gives allies a boon, its
profession and specialization, the boons, and its trigger read from the trait's description; and, for the kinds of
skills those triggers name, which skills are of that kind.

Writes src/TraitBoons.inc.

Usage:
    python tools/gen_trait_boons.py
"""
import json
import os
import re
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
BOONS = ["Might", "Fury", "Quickness", "Alacrity", "Protection", "Regeneration", "Vigor", "Aegis", "Stability",
         "Swiftness", "Resistance", "Resolution"]  # Analysis::kBoonNames order
PROFESSIONS = {"Guardian": 1, "Warrior": 2, "Engineer": 3, "Ranger": 4, "Thief": 5, "Elementalist": 6, "Mesmer": 7,
               "Necromancer": 8, "Revenant": 9}
# Triggers, as in Analysis.cpp
T_OTHER, T_DODGE, T_SWAP, T_HEAL, T_ELITE, T_KIND, T_INTERVAL, T_GRANT = range(8)
# Read by hand where the description names no action the log shows as one (the user's builds, 2026-10-02):
# Stalwart Speed gives quickness with every aegis or stability the Firebrand grants
OVERRIDES = {2076: (T_GRANT, "Aegis,Stability")}
WEAPONS = ["Axe", "Dagger", "Mace", "Pistol", "Scepter", "Sword", "Focus", "Shield", "Torch", "Warhorn", "Greatsword",
           "Hammer", "Longbow", "Rifle", "Shortbow", "Staff", "Spear", "Trident", "Speargun"]


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "FightReview/0.1"})
    return json.loads(urllib.request.urlopen(req, timeout=60).read())


def paged(endpoint):
    out, page = [], 0
    while True:
        try:
            batch = get(f"https://api.guildwars2.com/v2/{endpoint}?page={page}&page_size=200")
        except urllib.error.HTTPError:
            break
        if not batch:
            break
        out += batch
        page += 1
    return out


def trigger(trait, kinds):
    """(trigger, argument) from the description; T_OTHER when it names nothing the log shows."""
    if trait["id"] in OVERRIDES:
        return OVERRIDES[trait["id"]]
    raw = trait.get("description", "")
    text = re.sub(r"<[^>]+>", "", raw).lower()
    for tagged in re.findall(r"@abilitytype>([^<]+)<", raw):
        word = tagged.strip().lower()
        if word.startswith("shatter"):
            return T_KIND, "Shatter"  # no API category: a Mesmer's profession skills 1 to 4
        for k in kinds:
            if word in (k.lower(), k.lower() + "s", k.lower() + "es"):
                return T_KIND, k
    if "dodge" in text:
        return T_DODGE, ""
    if "swap weapon" in text or "weapon swap" in text or "swapping weapon" in text or "swap to" in text:
        return T_SWAP, ""
    if "heal skill" in text or "healing skill" in text:
        return T_HEAL, ""
    if "elite skill" in text:
        return T_ELITE, ""
    for w in WEAPONS:
        if re.search(rf"\b{w.lower()} skills?\b", text):
            return T_KIND, w
    if "interval" in text or re.search(r"every \d+ seconds", text):
        return T_INTERVAL, ""
    return T_OTHER, ""


def main():
    specs = {s["id"]: s for s in json.load(open(os.path.join(ROOT, "reference", "specializations.json"), encoding="utf-8"))}
    ids = get("https://api.guildwars2.com/v2/traits")
    traits = []
    for i in range(0, len(ids), 200):
        traits += get("https://api.guildwars2.com/v2/traits?ids=" + ",".join(map(str, ids[i:i + 200])))
    skills = paged("skills")
    # the kinds a skill can be: its categories (Mantra, Shout, Signet, ...)
    kinds = sorted({c for s in skills for c in s.get("categories", [])})

    rows, wanted = [], set()
    for t in traits:
        spec = specs.get(t.get("specialization"))
        if not spec or spec["profession"] not in PROFESSIONS:
            continue
        facts = (t.get("facts") or []) + (t.get("traited_facts") or [])
        mask = 0
        for f in facts:
            if f.get("type") in ("Buff", "PrefixedBuff") and f.get("status") in BOONS:
                mask |= 1 << BOONS.index(f["status"])
        if not mask:
            continue
        # only traits that reach others explain a boon given to someone else ("Gain might" is the trait holder's own)
        plain = re.sub(r"<[^>]+>", "", t.get("description", "")).lower()
        if t["id"] not in OVERRIDES and not re.search(r"\ball(y|ies)\b|\bparty\b|\bsquad\b|multiple targets", plain):
            continue
        trig, arg = trigger(t, kinds)
        if trig == T_KIND:
            wanted.add(arg)
        rows.append((t["id"], PROFESSIONS[spec["profession"]], spec["id"] if spec["elite"] else 0, mask, trig, arg, t["name"], t.get("icon", "")))

    kind_rows = []
    for s in skills:
        if not s.get("professions"):
            continue
        for k in s.get("categories", []):
            if k in wanted:
                kind_rows.append((s["id"], k))
        if s.get("weapon_type") in wanted:
            kind_rows.append((s["id"], s["weapon_type"]))
    # shatters: a Mesmer's profession skills 1 to 4, and Elite Insights' shatter seen from its effect (InstantCasts.cpp)
    if "Shatter" in wanted:
        for s in skills:
            if s.get("professions") == ["Mesmer"] and s.get("slot") in ("Profession_1", "Profession_2", "Profession_3", "Profession_4"):
                kind_rows.append((s["id"], "Shatter"))
        kind_rows.append((-900, "Shatter"))
    kind_rows = sorted(set(kind_rows))

    with open(os.path.join(ROOT, "src", "TraitBoons.inc"), "w", encoding="utf-8") as fh:
        fh.write("// Generated by tools/gen_trait_boons.py from the GW2 API: traits that give boons, their profession and\n")
        fh.write("// specialization (0: a core line), the boons (Analysis::kBoonNames bits), the trigger read from the description\n")
        fh.write("// (0 other, 1 dodge, 2 weapon swap, 3 heal skill, 4 elite skill, 5 a kind of skill, 6 each interval, 7 granting\n")
        fh.write("// the boons named), its argument, name and icon; then the skills of each kind a trigger names. Do not edit.\n")
        fh.write("struct TraitBoon { int32_t Trait; uint32_t Prof; uint32_t Spec; uint32_t Boons; uint8_t Trigger; const char* Arg; const char* Name; const char* Icon; };\n")
        fh.write("const TraitBoon kTraitBoons[] = {\n")
        for r in rows:
            fh.write(f"\t{{{r[0]}, {r[1]}, {r[2]}, {r[3]}, {r[4]}, {json.dumps(r[5])}, {json.dumps(r[6], ensure_ascii=False)}, {json.dumps(r[7])}}},\n")
        fh.write("};\n")
        fh.write("struct SkillKind { int32_t Skill; const char* Kind; };\n")
        fh.write("const SkillKind kSkillKinds[] = {\n")
        for sid, k in kind_rows:
            fh.write(f"\t{{{sid}, {json.dumps(k)}}},\n")
        fh.write("};\n")
    by = {}
    for r in rows:
        by[r[4]] = by.get(r[4], 0) + 1
    print(f"{len(traits)} traits, {len(rows)} give boons; by trigger {dict(sorted(by.items()))}; kinds {sorted(wanted)}; {len(kind_rows)} skills of those kinds")


if __name__ == "__main__":
    main()
