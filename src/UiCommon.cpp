#include "UiCommon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Icons.h"
#include "SkillIcons.h"

namespace Ui
{
	// Blue = you / us, orange = the enemy, grey = the compared player (checked with the dataviz skill's validator
	// on the #141619 window surface: blue/orange pass all-pairs, the grey steps clear the normal-vision floor)
	const ImVec4 kMuted(0.54f, 0.53f, 0.51f, 1.0f);
	const ImU32 kYou = IM_COL32(0x39, 0x87, 0xe5, 255);
	const ImU32 kPeer = IM_COL32(0x6b, 0x70, 0x78, 255);
	const ImU32 kPeerTick = IM_COL32(0xa8, 0xad, 0xb3, 255);
	const ImU32 kEnemy = IM_COL32(0xd9, 0x59, 0x26, 255);
	const ImU32 kTrack = IM_COL32(0x2c, 0x2f, 0x35, 255);
	const ImU32 kLaneBg = IM_COL32(0x10, 0x11, 0x14, 255);
	const ImU32 kOurBand = IM_COL32(57, 135, 229, 60);
	const ImU32 kEnemyBand = IM_COL32(217, 89, 38, 70);
	const ImU32 kEnemyPre = IM_COL32(217, 89, 38, 30);

	State& S()
	{
		static State s;
		return s;
	}

	// ---- formatting -----------------------------------------------------------------------------------------------

	// 400123 -> 400k, 7833 -> 7.8k, 999 -> 999, 12.34 -> 12.3, 0.5 -> 0.50, whole numbers stay whole
	std::string Num(double aValue)
	{
		char buf[32];
		double a = std::fabs(aValue);
		if (a >= 1e6) { std::snprintf(buf, sizeof(buf), "%.2fM", aValue / 1e6); }
		else if (a >= 1e5) { std::snprintf(buf, sizeof(buf), "%.0fk", aValue / 1e3); }
		else if (a >= 1e3) { std::snprintf(buf, sizeof(buf), "%.1fk", aValue / 1e3); }
		else if (a >= 100 || aValue == std::floor(aValue)) { std::snprintf(buf, sizeof(buf), "%.0f", aValue); }
		else if (a >= 10) { std::snprintf(buf, sizeof(buf), "%.1f", aValue); }
		else { std::snprintf(buf, sizeof(buf), "%.2f", aValue); }
		return buf;
	}

	std::string Lower(std::string aText)
	{
		for (char& ch : aText) { if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch - 'A' + 'a'); } }
		return aText;
	}

	std::string Clock(const std::string& aStamp) // yyyymmdd-hhmmss -> hh:mm
	{
		return aStamp.size() >= 13 ? aStamp.substr(9, 2) + ":" + aStamp.substr(11, 2) : aStamp;
	}

	std::string Duration(int64_t aMs)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(aMs / 60000), static_cast<long long>(aMs / 1000 % 60));
		return buf;
	}

	double PerS(const Player& aPlayer, double aValue)
	{
		return aPlayer.ActiveMs > 0 ? aValue * 1000.0 / aPlayer.ActiveMs : 0.0;
	}

	// "62% (48 of 77)", or "-" without enough evidence
	std::string Share(int aPart, int aWhole, int aMinimum)
	{
		if (aWhole < aMinimum || aWhole <= 0) { return "-"; }
		return std::to_string(int(100.0 * aPart / aWhole + 0.5)) + "% (" + std::to_string(aPart) + " of " + std::to_string(aWhole) + ")";
	}

	ImVec4 ProfessionColor(uint32_t aProf)
	{
		static const ImVec4 kColors[] = {ImVec4(0.8f, 0.8f, 0.8f, 1), ImVec4(0.45f, 0.76f, 0.85f, 1),
			ImVec4(1.0f, 0.82f, 0.40f, 1), ImVec4(0.82f, 0.61f, 0.35f, 1), ImVec4(0.55f, 0.86f, 0.51f, 1),
			ImVec4(0.75f, 0.56f, 0.58f, 1), ImVec4(0.96f, 0.54f, 0.53f, 1), ImVec4(0.71f, 0.47f, 0.84f, 1),
			ImVec4(0.32f, 0.65f, 0.44f, 1), ImVec4(0.82f, 0.43f, 0.35f, 1)};
		return aProf < 10 ? kColors[aProf] : kColors[0];
	}

	// ---- metrics --------------------------------------------------------------------------------------------------

	const std::vector<Metric>& Metrics()
	{
		static std::vector<Metric> m = []
		{
			std::vector<Metric> v;
			v.push_back({"Healing", K_Amount, [](const Player& p) { return double(p.Heal); }, [](const SkillRow& r) { return double(r.Heal); }, true});
			v.push_back({"Barrier", K_Amount, [](const Player& p) { return double(p.Barrier); }, [](const SkillRow& r) { return double(r.Barrier); }, true});
			v.push_back({"Damage to players", K_Amount, [](const Player& p) { return double(p.Damage); }, [](const SkillRow& r) { return double(r.Damage); }});
			v.push_back({"Damage to all", K_Amount, [](const Player& p) { return double(p.DamageAll); }, [](const SkillRow& r) { return double(r.DamageAll); }});
			v.push_back({"Strips", K_Count, [](const Player& p) { return double(p.Strips); }, [](const SkillRow& r) { return double(r.Strips); }});
			v.push_back({"Cleanses", K_Count, [](const Player& p) { return double(p.Cleanses); }, [](const SkillRow& r) { return double(r.Cleanses); }});
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				v.push_back({std::string(Analysis::kBoonNames[b]) + " to subgroup", K_Boon, [b](const Player& p) { return p.BoonGroupS[b]; },
					[b](const SkillRow& r) { return r.BoonGroupS[b]; }, false, b == 0 || b == Analysis::kStability});
			}
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				v.push_back({std::string(Analysis::kBoonNames[b]) + " to squad", K_Boon, [b](const Player& p) { return p.BoonSquadS[b]; },
					[b](const SkillRow& r) { return r.BoonSquadS[b]; }, false, b == 0 || b == Analysis::kStability});
			}
			return v;
		}();
		return m;
	}

	// Boons: seconds given per second alive = allies kept covered (queued boons) or stacks kept on others (intensity)
	const char* RateUnit(const Metric& m)
	{
		switch (m.What)
		{
		case K_Amount: return "/s";
		case K_Count: return "/min";
		default: return m.Intensity ? "stacks" : "allies";
		}
	}

	double Rate(const Metric& m, const Player& p, double aValue)
	{
		return m.What == K_Count ? PerS(p, aValue) * 60.0 : PerS(p, aValue);
	}

	bool Known(const Metric& m, const Player& p) { return !m.NeedsHealing || p.HealKnown; }

	int TimingWindow(int aMetric)
	{
		if (aMetric == kMetricHeal || aMetric == kMetricBarrier || aMetric == kMetricCleanses) { return Analysis::T_AnsweringTheirs; }
		if (aMetric >= kMetricGroupBoon) { return Analysis::T_AheadOfTheirs; }
		return Analysis::T_IntoOurs;
	}

	const char* WindowLabel(int aWindow)
	{
		static const char* kLabels[] = {"in our spike", "before an enemy spike", "just after an enemy spike"};
		return kLabels[aWindow];
	}

	// ---- players --------------------------------------------------------------------------------------------------

	// A player summed over several rounds (same account and spec)
	Player Sum(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec,
		std::map<int32_t, std::string>& aNames)
	{
		Player out;
		bool first = true;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec) { continue; }
				if (first)
				{
					out.Name = p.Name; out.Account = p.Account; out.Spec = p.Spec; out.Subgroup = p.Subgroup;
					out.ProfId = p.ProfId; out.EliteId = p.EliteId; first = false;
				}
				out.Pov |= p.Pov;
				out.HealKnown |= p.HealKnown;
				out.ActiveMs += p.ActiveMs;
				out.Heal += p.Heal; out.HealDowned += p.HealDowned; out.Barrier += p.Barrier; out.Damage += p.Damage;
				out.DamageAll += p.DamageAll; out.Strips += p.Strips; out.Cleanses += p.Cleanses;
				out.Evades += p.Evades; out.Blocks += p.Blocks; out.Invulns += p.Invulns; out.Downs += p.Downs;
				out.Deaths += p.Deaths; out.CcTaken += p.CcTaken; out.CcTakenMs += p.CcTakenMs; out.CcNoStab += p.CcNoStab;
				out.DamageTaken += p.DamageTaken; out.HealToSelf += p.HealToSelf; out.HealToGroup += p.HealToGroup;
				out.HealToOthers += p.HealToOthers; out.DownedMs += p.DownedMs;
				out.DownSpans.insert(out.DownSpans.end(), p.DownSpans.begin(), p.DownSpans.end());
				out.StabEligible += p.StabEligible; out.StabCovered += p.StabCovered; out.StabReady += p.StabReady;
				out.StabAllyMs += p.StabAllyMs; out.StabSelfMs += p.StabSelfMs;
				for (int b = 0; b < Analysis::kBoons; b++) { out.BoonGroupS[b] += p.BoonGroupS[b]; out.BoonSquadS[b] += p.BoonSquadS[b]; }
				for (auto& [skill, row] : p.Skills)
				{
					auto& o = out.Skills[skill];
					o.Casts += row.Casts; o.Heal += row.Heal; o.Barrier += row.Barrier; o.Damage += row.Damage;
					o.DamageAll += row.DamageAll; o.Strips += row.Strips; o.Cleanses += row.Cleanses;
					o.Interrupted += row.Interrupted; o.Cancelled += row.Cancelled;
					for (int b = 0; b < Analysis::kBoons; b++) { o.BoonGroupS[b] += row.BoonGroupS[b]; o.BoonSquadS[b] += row.BoonSquadS[b]; }
					for (int k = 0; k < Analysis::T_Count; k++) { o.Timing[k] += row.Timing[k]; }
				}
			}
			for (auto& [id, name] : f->SkillNames) { aNames.emplace(id, name); }
		}
		return out;
	}

	// Group generation over several rounds: each round's value weighted by its length
	double GroupGenOver(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec, int aBoon)
	{
		double weighted = 0, total = 0;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec) { continue; }
				weighted += f->GroupGeneration(p, aBoon) * f->DurationMs;
				total += double(f->DurationMs);
			}
		}
		return total > 0 ? weighted / total : 0.0;
	}

	// Stability first (it is the support's job even when they also heal), then healing, else damage
	Role RoleOf(const Player& p)
	{
		if (p.ActiveMs > 0 && p.StabAllyMs >= 2 * p.ActiveMs) { return R_Stab; }
		if (p.HealKnown && p.Heal > p.Damage) { return R_Heal; }
		return R_Damage;
	}

	int CastsCutShort(const Player& p)
	{
		int n = 0;
		for (auto& [s, r] : p.Skills) { n += r.Interrupted + r.Cancelled; }
		return n;
	}

	int64_t DeadMs(const Player& p)
	{
		int64_t ms = 0;
		for (const Analysis::Span& s : p.DownSpans) { if (s.Dead) { ms += s.To - s.From; } }
		return ms;
	}

	const SkillRow* Row(const Player& p, int32_t aSkill)
	{
		auto it = p.Skills.find(aSkill);
		return it == p.Skills.end() ? nullptr : &it->second;
	}

	std::string Ctx::Name(int32_t aSkill) const
	{
		auto it = Names.find(aSkill);
		return it != Names.end() ? it->second : std::to_string(aSkill);
	}

	std::string Ctx::PeerLabel() const
	{
		if (SameSpec) { return You.Spec; }
		return MyRole == R_Heal ? "healer" : MyRole == R_Stab ? "stability support" : "damage player";
	}

	Ctx BuildCtx(const std::vector<FightPtr>& aFights, int aIndex, bool aAllRounds)
	{
		Ctx c;
		State& s = S();
		c.Fights = &aFights;
		c.Index = aIndex;
		c.F = aFights[aIndex].get();
		c.OneRound = !aAllRounds;
		c.Scope = aAllRounds ? aFights : std::vector<FightPtr>{aFights[aIndex]};
		if (c.F->Pov < 0) { return c; }
		c.MeRaw = &c.F->Players[c.F->Pov];
		c.You = Sum(c.Scope, c.MeRaw->Account, c.MeRaw->Spec, c.Names);
		// Role from all your rounds on this spec: one quiet round (or one without Healing Stats) doesn't change it
		std::map<int32_t, std::string> ignore;
		c.MyRole = RoleOf(Sum(aFights, c.MeRaw->Account, c.MeRaw->Spec, ignore));
		c.RoleMetric = c.MyRole == R_Heal ? kMetricHeal : c.MyRole == R_Stab ? kMetricGroupBoon + Analysis::kStability : kMetricDamage;

		// Peers: the same spec; nobody else on it -> your role on any spec
		std::map<std::string, bool> seen;
		for (const FightPtr& fp : c.Scope)
		{
			for (const Player& p : fp->Players)
			{
				if (p.Spec != c.MeRaw->Spec || p.Account == c.MeRaw->Account || seen[p.Account]) { continue; }
				seen[p.Account] = true;
				c.Peers.push_back(Sum(c.Scope, p.Account, p.Spec, c.Names));
			}
		}
		if (c.Peers.empty())
		{
			c.SameSpec = false;
			for (const FightPtr& fp : c.Scope)
			{
				for (const Player& p : fp->Players)
				{
					std::string key = p.Account + "|" + p.Spec;
					if (p.Account == c.MeRaw->Account || seen[key] || RoleOf(p) != c.MyRole) { continue; }
					seen[key] = true;
					c.Peers.push_back(Sum(c.Scope, p.Account, p.Spec, c.Names));
				}
			}
		}
		const Metric& m = Metrics()[c.RoleMetric];
		std::sort(c.Peers.begin(), c.Peers.end(), [&](const Player& a, const Player& b)
		{
			if (Known(m, a) != Known(m, b)) { return Known(m, a); }
			return Rate(m, a, m.Total(a)) > Rate(m, b, m.Total(b));
		});
		for (const Player& p : c.Peers) { if (p.Account == s.VsAccount) { c.Vs = &p; } }
		if (!c.Vs && !c.Peers.empty()) { c.Vs = &c.Peers[0]; }
		if (c.Vs)
		{
			for (const Player& p : c.F->Players) { if (p.Account == c.Vs->Account && p.Spec == c.Vs->Spec) { c.VsRaw = &p; } }
		}
		return c;
	}

	// ---- why a skill gave less ----------------------------------------------------------------------------------------

	Why Explain(const Player& you, const Player& vs, const std::string& aVsName, int aMetric, int32_t aSkill, bool aOneRound)
	{
		const Metric& m = Metrics()[aMetric];
		const std::string unit = RateUnit(m);
		const SkillRow* ry = Row(you, aSkill);
		const SkillRow* rt = Row(vs, aSkill);
		double vy = ry ? m.PerSkill(*ry) : 0, vt = rt ? m.PerSkill(*rt) : 0;
		double rateY = Rate(m, you, vy), rateT = Rate(m, vs, vt);
		int cy = ry ? ry->Casts : 0, ct = rt ? rt->Casts : 0;
		Why w;
		w.Gap = rateT - rateY;
		auto output = [&]
		{
			w.You = rateY; w.Them = rateT; w.Unit = Lower(m.Name) + (m.What == K_Boon ? "" : " " + unit);
			w.YouText = Num(rateY); w.ThemText = Num(rateT);
		};
		auto counts = [&](double aYou, double aThem, const std::string& aUnit)
		{
			w.You = aYou; w.Them = aThem; w.Unit = aUnit; w.YouText = Num(aYou); w.ThemText = Num(aThem);
		};
		const std::string cost = Num(w.Gap) + " " + unit;
		if (w.Gap <= 0.0)
		{
			w.Word = rateY > 1.05 * rateT ? "you got more" : "about the same";
			w.Sentence = "You got " + Num(rateY) + " " + unit + " from it, " + aVsName + " " + Num(rateT) + ".";
			output();
			return w;
		}
		if (cy == 0 && ct == 0)
		{
			w.Word = "no casts";
			w.Sentence = "No casts: it comes from a boon, trait, relic or sigil. " + aVsName + " got " + cost + " more from it.";
			output();
			return w;
		}
		if (cy == 0)
		{
			w.Word = "not used";
			w.Sentence = "You didn't use it; it gave " + aVsName + " " + Num(rateT) + " " + unit + ".";
			counts(0, ct, "casts");
			return w;
		}
		int cutY = ry->Interrupted + ry->Cancelled, cutT = rt ? rt->Interrupted + rt->Cancelled : 0;
		double shareCutY = double(cutY) / cy, shareCutT = ct ? double(cutT) / ct : 0.0;
		if (shareCutY >= 0.25 && shareCutY >= shareCutT + 0.15)
		{
			w.Word = "cut short";
			w.Sentence = std::to_string(cutY) + " of your " + std::to_string(cy) + " casts were cut short, " +
				std::to_string(ry->Interrupted) + " of them by CC, a down or death.";
			counts(cutY, cutT, "casts cut short");
			return w;
		}
		int k = TimingWindow(aMetric);
		double inY = double(ry->Timing[k]) / cy, inT = ct ? double(rt->Timing[k]) / ct : 0.0;
		if (ct >= 3 && inT - inY >= 0.25)
		{
			w.Word = k == Analysis::T_AnsweringTheirs ? "off-beat" : k == Analysis::T_AheadOfTheirs ? "cast late" : "off the spike";
			w.Sentence = "Same skill, different moment: " + aVsName + " cast " + std::to_string(rt->Timing[k]) + " of " +
				std::to_string(ct) + " " + WindowLabel(k) + ", you " + std::to_string(ry->Timing[k]) + " of " + std::to_string(cy) + ".";
			counts(ry->Timing[k], rt->Timing[k], std::string("casts ") + WindowLabel(k));
			return w;
		}
		double cpmY = PerS(you, cy) * 60, cpmT = PerS(vs, ct) * 60;
		double perY = vy / cy, perT = ct ? vt / ct : 0.0;
		if (cpmT > 1.15 * cpmY)
		{
			w.Word = "fewer casts";
			w.Sentence = aOneRound ? std::to_string(ct - cy) + " fewer casts cost you " + cost + "."
				: "Fewer casts, " + Num(cpmY) + " against " + Num(cpmT) + " a minute, cost you " + cost + ".";
			if (perY > 1.05 * perT) { w.Sentence += " Each of yours did more than theirs."; }
			if (aOneRound) { counts(cy, ct, "casts"); } else { counts(cpmY, cpmT, "casts /min"); }
			return w;
		}
		if (perT > 1.15 * perY)
		{
			w.Word = "less per cast";
			w.Sentence = "Your casts did less each: " + Num(perY) + " against " + Num(perT) + " per cast.";
			counts(perY, perT, "per cast");
			return w;
		}
		w.Word = "a bit less";
		w.Sentence = "Close: a few fewer casts and a little less per cast cost you " + cost + ".";
		output();
		return w;
	}

	// ---- drawing ----------------------------------------------------------------------------------------------------------

	void SpecIcon(const Player& p)
	{
		float size = ImGui::GetTextLineHeight();
		if (void* icon = Icons::Get(p.ProfId, p.EliteId)) { ImGui::Image(icon, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ProfessionColor(p.ProfId)); }
		else { ImGui::Dummy(ImVec2(size, size)); }
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", p.Spec.c_str()); }
		ImGui::SameLine(0, 4);
	}

	void SkillIcon(int32_t aSkill, const std::string& aName)
	{
		float size = ImGui::GetTextLineHeight();
		if (void* icon = SkillIcons::Get(aSkill, aName)) { ImGui::Image(icon, ImVec2(size, size)); }
		else { ImGui::Dummy(ImVec2(size, size)); }
		ImGui::SameLine(0, 4);
	}

	void Headers(const std::vector<std::pair<const char*, const char*>>& aColumns)
	{
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < static_cast<int>(aColumns.size()); c++)
		{
			ImGui::TableSetColumnIndex(c);
			ImGui::TableHeader(aColumns[c].first);
			if (aColumns[c].second && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", aColumns[c].second); }
		}
	}

	void Cell(const std::string& aText, const ImVec4* aColor)
	{
		ImGui::TableNextColumn();
		if (aColor) { ImGui::TextColored(*aColor, "%s", aText.c_str()); }
		else { ImGui::TextUnformatted(aText.c_str()); }
	}

	// A number, right-aligned in its column so units sit under units
	void NumCell(const std::string& aText, const ImVec4* aColor)
	{
		ImGui::TableNextColumn();
		float width = ImGui::CalcTextSize(aText.c_str()).x;
		float avail = ImGui::GetContentRegionAvail().x;
		if (avail > width) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width); }
		if (aColor) { ImGui::TextColored(*aColor, "%s", aText.c_str()); }
		else { ImGui::TextUnformatted(aText.c_str()); }
	}

	// A value with a square-ended bar behind it, the bar's length relative to aMax
	void BarCell(double aValue, double aMax, ImU32 aColor, const std::string& aText, bool aOutline)
	{
		ImGui::TableNextColumn();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		float width = ImGui::GetContentRegionAvail().x;
		float h = ImGui::GetTextLineHeight();
		if (aMax > 0 && aValue > 0)
		{
			float w = static_cast<float>(width * std::min(1.0, aValue / aMax));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(ImVec2(pos.x, pos.y + 1), ImVec2(pos.x + w, pos.y + h - 1), aColor);
			if (aOutline) { dl->AddRect(ImVec2(pos.x, pos.y + 1), ImVec2(pos.x + w, pos.y + h - 1), ImGui::GetColorU32(ImGuiCol_Text)); }
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 3, pos.y));
		ImGui::TextUnformatted(aText.c_str());
	}

	void Rect(ImDrawList* aList, ImVec2 aPos, float aWidth, float aHeight, ImU32 aColor)
	{
		if (aWidth >= 0.5f && aHeight > 0) { aList->AddRectFilled(aPos, ImVec2(aPos.x + aWidth, aPos.y + aHeight), aColor); }
	}

	void SmallText(ImDrawList* aList, ImVec2 aPos, ImU32 aColor, const std::string& aText)
	{
		aList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f, aPos, aColor, aText.c_str());
	}

	void Key(ImU32 aColor, const char* aLabel)
	{
		float h = ImGui::GetTextLineHeight();
		ImVec2 p = ImGui::GetCursorScreenPos();
		Rect(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + h * 0.3f), h * 0.9f, h * 0.4f, aColor);
		ImGui::Dummy(ImVec2(h * 0.9f, h));
		ImGui::SameLine(0, 4);
		ImGui::TextColored(kMuted, "%s", aLabel);
		ImGui::SameLine(0, 12);
	}

	void Answer(const std::string& aText)
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(aText.c_str());
		ImGui::PopTextWrapPos();
	}

	void Lane(const Fight& f, const std::vector<int32_t>& aTimes, ImU32 aTick, float aWidth, float aHeight, int aWindow)
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
		auto x = [&](double aMs) { return p.x + static_cast<float>(std::clamp(aMs / span, 0.0, 1.0)) * aWidth; };
		dl->AddRectFilled(p, ImVec2(p.x + aWidth, p.y + aHeight), kLaneBg);
		for (int64_t t : f.OurSpikesMs) { dl->AddRectFilled(ImVec2(x(t - 2000.0), p.y), ImVec2(x(t + 2000.0), p.y + aHeight), kOurBand); }
		for (int64_t t : f.TheirSpikesMs)
		{
			if (aWindow == Analysis::T_AheadOfTheirs) { dl->AddRectFilled(ImVec2(x(t - 4000.0), p.y), ImVec2(x(double(t)), p.y + aHeight), kEnemyPre); }
			dl->AddRectFilled(ImVec2(x(double(t)), p.y), ImVec2(x(t + 3000.0), p.y + aHeight), kEnemyBand);
		}
		for (int32_t t : aTimes) { float tx = x(t); dl->AddRectFilled(ImVec2(tx, p.y + 1), ImVec2(tx + 2, p.y + aHeight - 1), aTick); }
		ImGui::Dummy(ImVec2(aWidth, aHeight));
	}

	void TimeAxis(const Fight& f, float aX, float aWidth)
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImU32 col = ImGui::GetColorU32(kMuted);
		for (int i = 0; i <= 4; i++)
		{
			std::string label = Duration(f.DurationMs * i / 4);
			float w = ImGui::CalcTextSize(label.c_str()).x * 0.85f;
			float lx = aX + aWidth * i / 4 - (i == 0 ? 0 : i == 4 ? w : w / 2);
			SmallText(dl, ImVec2(lx, p.y), col, label);
		}
		ImGui::Dummy(ImVec2(aWidth, ImGui::GetTextLineHeight()));
	}

	void SkillDetail(const Ctx& c, int aMetric, int32_t aSkill)
	{
		if (!c.Vs) { ImGui::TextColored(kMuted, "Nobody to compare with in this round."); return; }
		const Metric& m = Metrics()[aMetric];
		const std::string vsName = c.Vs->Name;
		Why w = Explain(c.You, *c.Vs, vsName, aMetric, aSkill, c.OneRound);
		Answer(w.Sentence);

		const SkillRow* ry = Row(c.You, aSkill);
		const SkillRow* rt = Row(*c.Vs, aSkill);
		double vy = ry ? m.PerSkill(*ry) : 0, vt = rt ? m.PerSkill(*rt) : 0;
		double rateY = Rate(m, c.You, vy), rateT = Rate(m, *c.Vs, vt);
		ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
		if (ImGui::BeginTable("detail", 6, flags))
		{
			std::string head = m.Name + " " + RateUnit(m);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 170);
			ImGui::TableSetupColumn(head.c_str(), ImGuiTableColumnFlags_WidthFixed, 200);
			for (int i = 0; i < 4; i++) { ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80); }
			Headers({{"", nullptr}, {head.c_str(), "From this skill"}, {"Casts", nullptr}, {"Casts /min", "Per minute alive"},
				{"Per cast", "Output of one cast"}, {"Cut short", "Stopped before it went off"}});
			auto line = [&](const Player& p, const SkillRow* r, double aRate, ImU32 aColor, const std::string& aName)
			{
				ImGui::TableNextRow();
				Cell(aName);
				BarCell(aRate, std::max(rateY, rateT), aColor, Num(aRate));
				int casts = r ? r->Casts : 0;
				NumCell(casts ? std::to_string(casts) : "-");
				NumCell(casts ? Num(PerS(p, casts) * 60) : "-");
				NumCell(casts && r ? Num(m.PerSkill(*r) / casts) : "-");
				NumCell(casts && r ? std::to_string(r->Interrupted + r->Cancelled) : "-");
			};
			line(c.You, ry, rateY, kYou, "You");
			line(*c.Vs, rt, rateT, kPeer, vsName);
			ImGui::EndTable();
		}

		// When you each cast it (one round: the lanes need the round's spikes)
		int k = TimingWindow(aMetric);
		const SkillRow* lY = c.MeRaw ? Row(*c.MeRaw, aSkill) : nullptr;
		const SkillRow* lT = c.VsRaw ? Row(*c.VsRaw, aSkill) : nullptr;
		if ((lY && lY->Casts) || (lT && lT->Casts))
		{
			ImGui::Spacing();
			ImGui::TextColored(kMuted, "When you each cast it");
			ImGui::SameLine(0, 20);
			Key(kOurBand, "our spike");
			Key(k == Analysis::T_AheadOfTheirs ? kEnemyPre : kEnemyBand, k == Analysis::T_AheadOfTheirs ? "4 s before an enemy spike" : "enemy spike and 3 s after");
			ImGui::NewLine();
			if (!c.OneRound) { ImGui::TextColored(kMuted, "Time lines show one round at a time: pick This round."); }
			else
			{
				const float labelW = 170, laneW = std::max(200.0f, ImGui::GetContentRegionAvail().x - labelW - 110);
				auto lane = [&](const char* aName, const SkillRow* r, ImU32 aTick)
				{
					ImGui::TextUnformatted(aName);
					ImGui::SameLine(labelW);
					static const std::vector<int32_t> kNone;
					Lane(*c.F, r ? r->CastMs : kNone, aTick, laneW, ImGui::GetTextLineHeight(), k);
					ImGui::SameLine();
					if (r && r->Casts) { ImGui::Text("%d of %d", r->Timing[k], r->Casts); } else { ImGui::TextColored(kMuted, "-"); }
				};
				lane("You", lY, kYou);
				lane(vsName.c_str(), lT, kPeerTick);
				TimeAxis(*c.F, ImGui::GetWindowPos().x - ImGui::GetScrollX() + labelW, laneW); // SameLine(x) is from the window's edge
				ImGui::TextColored(kMuted, "Right: casts %s, of all casts.", WindowLabel(k));
			}
		}
		if (ImGui::SmallButton("Open in Compare"))
		{
			S().SwitchTo = T_Compare; S().Metric = aMetric; S().CompareOpen = aSkill;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Show on the Fight time line")) { S().SwitchTo = T_Fight; }
	}
}
