// RoundWhy a round was lost or won, rebuilt on cause and effect. A round is decided by downs, so the view follows them, in four parts: allies downed
// (and what each downed ally had in the 3 s before: CC, immobilize, no protection, CC'd during a kite or ran past the squad, hurt), allies who died
// (and the revive skills there were), enemies downed (what allies did to them), enemies who died (the damage on their downs). Above them, where the
// round turned: the enemy pushes that cost the most. Each line: an icon, a few words, this round, won fights, who; the sentence and the evidence
// behind it on hover; the downs or skills behind it on a click.
//
// How the lines were chosen (Causes.h; data in notes/TODO.md #1): each ally down against the allies hit at the same moment
// who stayed up, over 6,275 downs in 550 fights (3 Sept to 6 Oct). What made the difference there, holding the rest fixed,
// is a cause; what only went with losing over a whole round (regeneration uptime, allies hitting in pushes, the first
// down) is not, and is gone. Odds and "won fights" values come from fights of the round's size (15v15 to 40+), from every
// player's logs at hand: Causes::SizeBand, src/CauseRefs.inc (tools/causal/gen_refs.py); counts scaled to the round's length.
#include "UiCommon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

#include "Causes.h"

namespace Ui
{
	namespace
	{
		const ImU32 kGold = IM_COL32(0xc9, 0xa2, 0x27, 255);
		const ImU32 kWhoInk = IM_COL32(0xa9, 0xb4, 0xc6, 255);
		std::string P(double v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; }
		std::string I(double v) { return std::to_string(static_cast<long>(std::lround(v))); }
		std::string S1(double v) { char b[24]; std::snprintf(b, sizeof b, "%.1f", v); return b; }
		std::string Plural(int n, const char* aOne, const char* aMany) { return std::to_string(n) + " " + (n == 1 ? aOne : aMany); }
		// "Rez skills ran out" -> "rez skills ran out", but "CC'd, then down" stays
		std::string Low(std::string s)
		{
			if (s.size() > 1 && std::islower(static_cast<unsigned char>(s[1]))) { s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0]))); }
			return s;
		}

		// Someone in a line's who column: an ally (name and class) or an enemy class; Extra on hover ("0:37")
		struct Who { std::string Name, Spec, Extra; };
		using Whos = std::vector<Who>;

		// ---- the lines -------------------------------------------------------------------------------------------------
		// the first six in Causes::Factor order
		enum LineId { L_Cc, L_Immob, L_NoProt, L_CcKite, L_RanPast, L_Hurt, L_RanOut, L_Unused, L_Fast, L_CcLanded, L_Focus, L_CcBefore, L_Cleave, L_Rallied, L_Count };
		enum Part { S_Downed, S_Died, S_EnemyDowned, S_EnemyDied, S_Count };
		// Won fights' values come from fights of the round's size (Causes::SizeBand, src/CauseRefs.inc): Median and Worse (the
		// won fights' worse quartile), Cost (downs the cause cost in a won fight, for the order), Lost (lost fights' middle).
		// Counts per 10 allies per minute (scaled to the round); shares and rates as they are. A line is marked only beyond
		// what won fights show: a count past the won fights' worse quartile (red past twice it and one more), a rate under
		// their lower quartile (red under the lost fights' middle). Inside won fights' range a line counts as fine, however it
		// compares with their middle.
		struct Def { int Part; const char* Label; bool MoreIsWorse; };
		const Def kDefs[L_Count] = {
			{S_Downed, "CC'd, then down", true},
			{S_Downed, "Immobilized, then down", true},
			{S_Downed, "No protection", true},
			{S_Downed, "CC'd during kite", true},
			{S_Downed, "Ran past the squad", true},
			{S_Downed, "Hurt coming in", true},
			{S_Died, "Rez skills ran out", true},
			{S_Died, "Rez skills not used", true},
			{S_Died, "Died within 1.5 s", true},
			{S_EnemyDowned, "Ally CC landed", false},
			{S_EnemyDowned, "Focus fire", false},
			{S_EnemyDowned, "CC before pushes", false},
			// time held equal: allies on a down in its first 1.5 s, the round's middle down; rallies: enemy downs up the
			// moment an ally they had hit died
			{S_EnemyDied, "Allies on enemy downs", false},
			{S_EnemyDied, "Rallied off ally deaths", true},
		};
		static_assert(L_Count == Causes::kRefLines, "src/CauseRefs.inc holds a value set per line, in this order");

		// One round, worked out once: its causes (no risk sets: the view needs each down's own state only) and summary
		struct RoundWhy
		{
			const Fight* F = nullptr;
			Causes::Round R;
			Causes::Summary S;
			double Scale = 1; // allies / 10 x minutes: what "per 10 allies per minute" becomes for this round
			int Result = 0;   // 1 won, -1 lost, 0 even (by downs)
			const Causes::SizeBand* Band = nullptr; // the odds and won fights of this round's size
		};
		const RoundWhy& WhyOf(const std::vector<FightPtr>& aFights, int aIndex)
		{
			static std::map<std::string, RoundWhy> cache;
			static uint64_t version = UINT64_MAX;
			if (version != DataVersion()) { cache.clear(); version = DataVersion(); }
			const Fight& f = *aFights[aIndex];
			auto it = cache.find(f.Stamp);
			if (it != cache.end()) { return it->second; }
			Causes::Night night;
			for (const FightPtr& g : aFights) { if (g) { night.Rounds.push_back(g.get()); } }
			night.Index = aIndex;
			RoundWhy w;
			w.F = &f;
			w.R = Causes::Compute(f, night, false);
			w.S = Causes::Summarize(f, w.R);
			w.Scale = std::max(0.05, f.SquadCount / 10.0 * f.DurationMs / 60000.0);
			w.Result = f.EnemyDowns > f.SquadDowns ? 1 : f.EnemyDowns < f.SquadDowns ? -1 : 0;
			w.Band = &Causes::BandOf(f.SquadCount);
			return cache.emplace(f.Stamp, std::move(w)).first->second;
		}
		int IndexOf(const Ctx& c) { return c.Index; }

		// A line's numbers for one round
		struct LineRow
		{
			int Id = 0;
			bool Known = false;
			double Value = 0;     // a count, a share or a rate
			double Expected = 0;  // won fights, for this round (counts scaled)
			int Verdict = 0;      // 0 fine, 1 a bit worse, 2 worse
			double Excess = 0;    // players it cost beyond a won fight (the order)
		};
		double ValueOf(const RoundWhy& w, int aId, bool* aKnown)
		{
			const Causes::Summary& s = w.S;
			*aKnown = true;
			switch (aId)
			{
			case L_Cc: case L_Immob: case L_NoProt: case L_CcKite: case L_RanPast: case L_Hurt: return s.With[aId - L_Cc];
			case L_RanOut: return s.DiedRanOut;
			case L_Unused: return s.DiedUnused;
			case L_Fast: return s.DiedFast;
			case L_CcLanded: *aKnown = s.CcLanded >= 0; return s.CcLanded;
			case L_Focus: *aKnown = s.Focus >= 0 && s.AllyPushes >= 1; return s.Focus;
			case L_CcBefore: *aKnown = s.CcBefore >= 0 && s.AllyPushes >= 2; return s.CcBefore;
			case L_Cleave: *aKnown = s.HittersEarly >= 0; return s.HittersEarly;
			case L_Rallied: return s.EnemyRallied;
			default: *aKnown = false; return 0;
			}
		}
		LineRow RowOf(const RoundWhy& w, int aId)
		{
			const Def& d = kDefs[aId];
			const Causes::LineRef& ref = w.Band->Lines[aId];
			LineRow r;
			r.Id = aId;
			r.Value = ValueOf(w, aId, &r.Known);
			if (!r.Known) { return r; }
			if (d.MoreIsWorse)
			{
				r.Expected = ref.Median * w.Scale;
				const double high = ref.Worse * w.Scale;
				r.Verdict = r.Value >= 2 && r.Value > 2 * high + 1 ? 2 : r.Value >= 1 && r.Value > high + 0.5 ? 1 : 0;
				r.Excess = aId <= L_Hurt ? w.S.Cost[aId - L_Cc] - ref.Cost * w.Scale : r.Value - r.Expected;
			}
			else
			{
				r.Expected = ref.Median;
				r.Verdict = r.Value < ref.Lost ? 2 : r.Value < ref.Worse ? 1 : 0;
				if (aId == L_Cleave)
				{
					// enemy kills it missed: the downs, at won fights' share of them dying, less the ones that rallied (their own line)
					const double share = w.S.EnemyDowns ? double(w.S.EnemyDeaths) / w.S.EnemyDowns : 0;
					r.Excess = std::max(0.0, w.S.EnemyDowns * std::max(0.0, w.Band->EnemyDied.Median - share) - w.S.EnemyRallied);
				}
				else { r.Excess = (ref.Median - r.Value) / std::max(1e-9, ref.Median); }
			}
			return r;
		}
		std::string Format(int aId, double v)
		{
			switch (aId)
			{
			case L_CcLanded: case L_Focus: return P(v);
			case L_CcBefore: case L_Cleave: return S1(v);
			default: return I(v);
			}
		}

		// ---- icons --------------------------------------------------------------------------------------------------------
		void StrikeOut(ImDrawList* dl, ImVec2 p, float s) { dl->AddLine(ImVec2(p.x + s * 0.15f, p.y + s * 0.85f), ImVec2(p.x + s * 0.85f, p.y + s * 0.15f), kEnemy, 2.0f); }
		void LineIcon(ImDrawList* dl, ImVec2 p, float s, int aId)
		{
			switch (aId)
			{
			case L_Cc: CcIconAt(dl, p, s, Analysis::CC_Knockdown); break;
			case L_Immob: IconAt(dl, p, s, -330, "Immobile"); break;
			case L_NoProt: BoonIconAt(dl, p, s, 4, true); break;
			case L_CcKite:
			{
				// the squad as a ring kiting back (a chevron behind it), one ally held where they stood (a stun's halo over them)
				const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
				dl->AddCircle(ImVec2(p.x + s * 0.44f, p.y + s * 0.62f), s * 0.22f, ink, 12, 1.5f);
				const ImVec2 tip(p.x + s * 0.04f, p.y + s * 0.62f);
				dl->AddLine(ImVec2(tip.x + s * 0.11f, tip.y - s * 0.13f), tip, ink, 1.5f);
				dl->AddLine(tip, ImVec2(tip.x + s * 0.11f, tip.y + s * 0.13f), ink, 1.5f);
				dl->AddCircleFilled(ImVec2(p.x + s * 0.84f, p.y + s * 0.42f), s * 0.13f, kEnemy);
				dl->AddCircle(ImVec2(p.x + s * 0.84f, p.y + s * 0.14f), s * 0.11f, kEnemy, 10, 1.2f);
				break;
			}
			case L_RanPast:
			{
				// the squad as a ring, one ally running out past it (a trail from the ring to them)
				const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
				dl->AddCircle(ImVec2(p.x + s * 0.28f, p.y + s * 0.64f), s * 0.22f, ink, 12, 1.5f);
				dl->AddLine(ImVec2(p.x + s * 0.46f, p.y + s * 0.50f), ImVec2(p.x + s * 0.72f, p.y + s * 0.32f), kEnemy, 1.5f);
				dl->AddCircleFilled(ImVec2(p.x + s * 0.82f, p.y + s * 0.24f), s * 0.13f, kEnemy);
				break;
			}
			case L_Hurt:
			{
				// a health bar a little over half full
				const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
				dl->AddRect(ImVec2(p.x + 1, p.y + s * 0.32f), ImVec2(p.x + s - 1, p.y + s * 0.68f), ink);
				dl->AddRectFilled(ImVec2(p.x + 2, p.y + s * 0.32f + 1), ImVec2(p.x + 2 + (s - 4) * 0.55f, p.y + s * 0.68f - 1), kEnemy);
				break;
			}
			case L_RanOut: IconAt(dl, p, s, -302, "Resurrect"); StrikeOut(dl, p, s); break;
			case L_Unused: IconAt(dl, p, s, -302, "Resurrect", kGold); break;
			case L_Fast: IconAt(dl, p, s, -301, "Downed", kEnemy); break;
			case L_CcLanded: IconAt(dl, p, s, -100 - static_cast<int>(Analysis::CC_Launch), Analysis::kCcIcons[Analysis::CC_Launch]); break;
			case L_Focus: IconAt(dl, p, s, -303, "Damage burst"); break;
			case L_CcBefore: IconAt(dl, p, s, -100 - static_cast<int>(Analysis::CC_Knockback), Analysis::kCcIcons[Analysis::CC_Knockback]); break;
			case L_Cleave: IconAt(dl, p, s, -301, "Downed", kYou); break;
			case L_Rallied:
				// an enemy down getting back up: the downed icon with a rising mark
				IconAt(dl, p, s, -301, "Downed", kEnemy);
				Mark(dl, ImVec2(p.x + s * 0.78f, p.y + s * 0.74f), s * 0.24f, 3, kEnemy);
				break;
			default: break;
			}
		}

		// ---- what each line says on hover, and who it names -------------------------------------------------------------
		// A line's hover: what the number counts, one line of evidence, where a click goes
		std::string Tip(const RoundWhy& w, const LineRow& r)
		{
			const Causes::Summary& s = w.S;
			// the evidence line: the odds in fights of this round's size ("In 20v20 fights they go down 2.9x as often")
			auto odds = [&](int aFactor, const char* aWho, const char* aMore = "")
			{
				return std::string(".\nIn ") + w.Band->Name + " fights " + aWho + " go down " + S1(w.Band->Odds[aFactor]) + "x as often" + aMore + ".\nClick: these downs";
			};
			switch (r.Id)
			{
			case L_Cc:
				return std::string("Down within 3 s of a CC, no stability then") + (s.CcStabStripped ? " (" + std::to_string(s.CcStabStripped) + " stripped first)" : std::string()) +
					odds(Causes::F_Cc, "CC'd allies");
			case L_Immob: return "Immobilized in the 3 s before the down" + odds(Causes::F_Immob, "they");
			case L_NoProt: return "No protection at the down" + odds(Causes::F_NoProt, "allies without it");
			case L_CcKite: return "CC'd, rooted or slowed as the squad kited back" + odds(Causes::F_CcKite, "they", ", on top of the CC");
			case L_RanPast: return "Ran 300+ past the squad on their own (not pulled)" + odds(Causes::F_RanPast, "they");
			case L_Hurt: return "Under 90% health 3 s before the down" + odds(Causes::F_Hurt, "they");
			case L_RanOut:
				return std::string("Died with 0-1 rez skills ready nearby") + (s.LastToolMs >= 0 ? " (last used " + Duration(s.LastToolMs) + ")" : std::string()) +
					".\nNone ready: 68% die; 3+ ready: 29%.\nClick: the revive skills";
			case L_Unused: return "Died with 2+ rez skills ready nearby (estimated).\nClick: the revive skills";
			case L_Fast: return "Died within 1.5 s of going down: no time to revive.";
			case L_CcLanded: return P(r.Value) + " of ally CC landed; enemy stability took the rest.\nEnemies CC'd go down 2.4x as often.\nClick: the round graph";
			case L_Focus: return P(r.Value) + " of push damage hit an enemy 10+ allies were on.\nFocused enemies go down 5x as often.\nClick: the round graph";
			case L_CcBefore: return "Ally CC landing in the 3 s before each push.\nClick: the round graph";
			case L_Cleave: return "Allies hitting a downed enemy in its first 1.5 s.\nWith 8+ on it 77% die; with 2 or fewer, 36%.\nClick: the round graph";
			case L_Rallied: return "Enemy downs up the moment an ally they'd hit died (a rally).";
			default: return std::string();
			}
		}

		Whos WhoOf(const RoundWhy& w, int aId)
		{
			const Fight& f = *w.F;
			Whos out;
			std::set<int> seen;
			auto add = [&](int aPlayer, const std::string& aExtra)
			{
				if (aPlayer < 0 || static_cast<size_t>(aPlayer) >= f.Players.size()) { return; }
				if (seen.insert(aPlayer).second) { out.push_back({f.Players[aPlayer].Name, f.Players[aPlayer].Spec, aExtra}); }
				else { for (Who& x : out) { if (x.Name == f.Players[aPlayer].Name) { x.Extra += ", " + aExtra; } } }
			};
			const auto& downs = w.R.AllyDowns;
			if (aId <= L_Hurt)
			{
				for (size_t i = 0; i < downs.size(); i++) { if (w.S.Flags[i] & (1u << (aId - L_Cc))) { add(downs[i].Player, Duration(downs[i].Ms)); } }
			}
			else if (aId == L_RanOut || aId == L_Fast)
			{
				for (const auto& d : downs)
				{
					if (!d.Died || d.AfterStop) { continue; }
					const bool fast = d.DownMs < 1500, ranOut = !fast && d.ToolsReady >= 0 && d.ToolsReady <= 1;
					if ((aId == L_Fast && fast) || (aId == L_RanOut && ranOut)) { add(d.Player, Duration(d.Ms)); }
				}
			}
			else if (aId == L_Unused)
			{
				for (int q : w.S.UnusedBy) { add(q, "had one ready"); }
			}
			else if (aId == L_CcLanded)
			{
				std::vector<std::pair<int, int>> by;
				for (size_t i = 0; i < f.Players.size(); i++) { if (f.Players[i].CcIntoStab > 0) { by.push_back({f.Players[i].CcIntoStab, static_cast<int>(i)}); } }
				std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.first > b.first; });
				for (auto& [n, i] : by)
				{
					const int all = n + static_cast<int>(f.Players[i].CcOutMs.size());
					add(i, std::to_string(n) + " of " + std::to_string(all) + " blocked");
				}
			}
			return out;
		}
		const char* WhoWord(int aId)
		{
			switch (aId)
			{
			case L_Unused: return "ready";
			case L_CcLanded: return "blocked";
			case L_RanOut: case L_Fast: return "died";
			default: return "";
			}
		}
		const char* WhoTip(int aId)
		{
			switch (aId)
			{
			case L_Unused: return "Had a revive skill ready within reach of an ally who died:";
			case L_CcLanded: return "Enemy stability took most of their CC (of their CC on enemy players):";
			case L_RanOut: case L_Fast: return "Died:";
			default: return "Went down (when):";
			}
		}

		// The who cell: class icons and names, as many as fit, then "+3"; all of them with their numbers on hover
		void People(ImDrawList* dl, int aId, const Whos& aWho)
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x, lh = ImGui::GetTextLineHeight(), icon = lh * 0.95f;
			ImGui::Dummy(ImVec2(w, lh));
			if (aWho.empty()) { return; }
			const ImU32 muted = ImGui::GetColorU32(kMuted);
			float x = p.x;
			if (*WhoWord(aId)) { dl->AddText(ImVec2(x, p.y), muted, WhoWord(aId)); x += ImGui::CalcTextSize(WhoWord(aId)).x + 6; }
			static std::map<std::string, Player> stubs;
			auto words = [&](const Who& e, size_t i) { return e.Name.empty() ? e.Extra : (i == 0 || e.Spec.empty()) ? e.Name : std::string(); };
			size_t shown = 0;
			for (; shown < aWho.size(); shown++)
			{
				const Who& e = aWho[shown];
				const std::string t = words(e, shown);
				const float ew = (e.Spec.empty() ? 0 : icon + (t.empty() ? 2 : 3)) + (t.empty() ? 0 : ImGui::CalcTextSize(t.c_str()).x + 8);
				const float more = shown + 1 < aWho.size() ? ImGui::CalcTextSize("+99").x : 0;
				if (shown > 0 && x + ew + more > p.x + w) { break; }
				if (!e.Spec.empty())
				{
					auto it = stubs.find(e.Spec);
					if (it == stubs.end()) { it = stubs.emplace(e.Spec, SpecPlayer(e.Spec)).first; }
					SpecIconAt(dl, ImVec2(x, p.y + (lh - icon) * 0.5f), icon, it->second);
					x += icon + (t.empty() ? 2 : 3);
				}
				if (!t.empty()) { dl->AddText(ImVec2(x, p.y), kWhoInk, t.c_str()); x += ImGui::CalcTextSize(t.c_str()).x + 8; }
			}
			if (shown < aWho.size()) { dl->AddText(ImVec2(x + 4, p.y), muted, ("+" + std::to_string(aWho.size() - shown)).c_str()); }
			if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + lh)) && ImGui::IsWindowHovered())
			{
				std::string tip = WhoTip(aId);
				for (const Who& e : aWho)
				{
					tip += "\n" + (e.Name.empty() ? e.Spec : e.Name + (e.Spec.empty() ? std::string() : " (" + e.Spec + ")"));
					if (!e.Extra.empty()) { tip += ": " + e.Extra; }
				}
				TipWrapped(tip);
			}
		}

		ImU32 VerdictColour(int v) { return v == 0 ? kYou : v == 1 ? kGold : kEnemy; }
		int VerdictShape(int v) { return v == 0 ? 1 : 2; }

		// A click on a line: the evidence behind it
		void Open(const RoundWhy& w, int aId)
		{
			State& s = S();
			if (aId <= L_Hurt) { Go(P_Downs); s.DeathFilter = 6; s.DeathCause = aId - L_Cc; s.DeathKey.clear(); return; }
			if (aId <= L_Fast) { Go(P_Revives); return; }
			Go(P_Round); s.SpikeOpen = -1; s.RoundView = 0;
			(void)w;
		}

		// A small item on one line: an icon and a short value, the words on hover; true when clicked
		bool Chip(const char* aId, float& x, float y, const std::function<void(ImDrawList*, ImVec2, float)>& aIcon, const std::string& aText, const std::string& aTip, bool* aHov = nullptr, ImU32 aInk = kWhoInk)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float lh = ImGui::GetTextLineHeight(), w = lh + 3 + ImGui::CalcTextSize(aText.c_str()).x;
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			const bool clicked = ImGui::InvisibleButton(aId, ImVec2(w, lh));
			const bool hov = ImGui::IsItemHovered();
			if (aHov) { *aHov |= hov; }
			aIcon(dl, ImVec2(x, y), lh);
			dl->AddText(ImVec2(x + lh + 3, y), hov ? ImGui::GetColorU32(ImGuiCol_Text) : aInk, aText.c_str());
			if (hov) { TipWrapped(aTip); }
			x += w + lh * 0.7f;
			return clicked;
		}

		// ---- this round ----------------------------------------------------------------------------------------------------
		// The lines that cost the most players, across the parts (enemies downed is told in its own terms, not players)
		std::vector<LineRow> Top(const RoundWhy& w, size_t aMax)
		{
			std::vector<LineRow> v;
			for (int id = 0; id < L_Count; id++)
			{
				if (kDefs[id].Part == S_EnemyDowned) { continue; }
				const LineRow r = RowOf(w, id);
				if (r.Known && r.Verdict > 0 && r.Excess >= 0.5) { v.push_back(r); }
			}
			std::sort(v.begin(), v.end(), [](const LineRow& a, const LineRow& b) { return a.Excess > b.Excess; });
			if (v.size() > aMax) { v.resize(aMax); }
			return v;
		}
		// A won round's lines that beat a won fight of its size by the most: a count won fights have (1+) that this round had a
		// quarter fewer of, a share or rate a quarter above them; the furthest first
		std::vector<LineRow> Best(const RoundWhy& w, size_t aMax)
		{
			std::vector<std::pair<double, LineRow>> best;
			for (int id = 0; id < L_Count; id++)
			{
				const LineRow r = RowOf(w, id);
				if (!r.Known || r.Verdict > 0) { continue; }
				const double margin = kDefs[id].MoreIsWorse ? (r.Expected >= 1 ? (r.Expected - r.Value) / r.Expected : 0)
					: (r.Expected > 0 ? (r.Value - r.Expected) / r.Expected : 0);
				if (margin >= 0.25) { best.push_back({margin, r}); }
			}
			std::sort(best.begin(), best.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			std::vector<LineRow> out;
			for (size_t i = 0; i < best.size() && i < aMax; i++) { out.push_back(best[i].second); }
			return out;
		}
		std::string Short(const RoundWhy&, const LineRow& r)
		{
			switch (r.Id)
			{
			case L_RanOut: case L_Unused: case L_Fast: return Plural(static_cast<int>(r.Value), "died", "died");
			case L_Cleave: return S1(r.Value) + " on each";
			case L_Rallied: return Plural(static_cast<int>(r.Value), "got up", "got up");
			default: return kDefs[r.Id].MoreIsWorse ? I(r.Value) : Format(r.Id, r.Value);
			}
		}

		// Where it turned: the enemy pushes that cost the most, in time order
		void Moments(const RoundWhy& w)
		{
			const Fight& f = *w.F;
			std::vector<const Causes::Wave*> pick;
			for (const Causes::Wave& wave : w.S.Waves) { if (wave.Downs >= 2 && pick.size() < 3) { pick.push_back(&wave); } }
			if (pick.empty()) { return; }
			std::sort(pick.begin(), pick.end(), [](auto* a, auto* b) { return a->From < b->From; });
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float lh = ImGui::GetTextLineHeight();
			ImVec2 o = ImGui::GetCursorScreenPos();
			SmallText(dl, ImVec2(o.x, o.y + 1), ImGui::GetColorU32(kMuted), "WHERE IT TURNED");
			ImGui::Dummy(ImVec2(1, lh));
			for (size_t i = 0; i < pick.size(); i++)
			{
				const Causes::Wave& m = *pick[i];
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float w0 = ImGui::GetContentRegionAvail().x;
				ImGui::PushID(static_cast<int>(i));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0x1d, 0x26, 0x36, 255));
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0x23, 0x2e, 0x42, 255));
				const bool clicked = ImGui::Selectable("##moment", false, ImGuiSelectableFlags_AllowItemOverlap, ImVec2(w0, lh));
				ImGui::PopStyleColor(2);
				const bool hov = ImGui::IsItemHovered();
				ImGui::PopID();
				float x = p.x;
				Mark(dl, ImVec2(x + lh * 0.4f, p.y + lh * 0.5f), lh * 0.3f, 4, kEnemy);
				x += lh;
				const std::string when = Duration(m.From) + (m.To - m.From >= 1500 ? "-" + Duration(m.To) : std::string());
				dl->AddText(ImVec2(x, p.y), ImGui::GetColorU32(kMuted), when.c_str());
				x += ImGui::CalcTextSize("0:00-0:00").x + lh * 0.5f;
				const std::string what = Plural(m.Downs, "down", "down") + (m.Deaths ? " \xc2\xb7 " + Plural(m.Deaths, "died", "died") : std::string());
				dl->AddText(ImVec2(x, p.y), ImGui::GetColorU32(ImGuiCol_Text), what.c_str());
				x += std::max(ImGui::CalcTextSize(what.c_str()).x, ImGui::CalcTextSize("88 down \xc2\xb7 88 died").x) + lh;
				auto chip = [&](const std::function<void(ImDrawList*, ImVec2, float)>& aIcon, const std::string& aText)
				{
					const float iw = aIcon ? lh + 3 : 0;
					if (aIcon) { aIcon(dl, ImVec2(x, p.y), lh); }
					dl->AddText(ImVec2(x + iw, p.y), kWhoInk, aText.c_str());
					x += iw + ImGui::CalcTextSize(aText.c_str()).x + lh * 0.7f;
				};
				if (m.Cc) { chip([](ImDrawList* d, ImVec2 q, float s) { CcIconAt(d, q, s, Analysis::CC_Knockdown); }, std::to_string(m.Cc) + " CC'd"); }
				if (m.StabStripped) { chip([](ImDrawList* d, ImVec2 q, float s) { BoonIconAt(d, q, s, Analysis::kStability, true); }, std::to_string(m.StabStripped) + " stripped"); }
				if (m.SubgroupDowns >= 2) { chip(nullptr, "sg " + std::to_string(m.Subgroup) + " lost " + std::to_string(m.SubgroupDowns)); }
				if (m.ToolsReady >= 0 && m.Deaths) { chip([](ImDrawList* d, ImVec2 q, float s) { IconAt(d, q, s, -302, "Resurrect"); }, std::to_string(m.ToolsReady) + " rez ready"); }
				if (hov) { TipWrapped("Enemy push " + when + ".\nClick: this push, broken down"); }
				if (clicked && m.Push >= 0 && static_cast<size_t>(m.Push) < f.TheirSpikesMs.size())
				{
					State& s = S();
					Go(P_Round);
					s.SpikeOpen = f.TheirSpikesMs[m.Push]; s.SpikeEnemy = true; s.SpikeStamp = f.Stamp; s.RoundView = 0;
				}
			}
			ImGui::Dummy(ImVec2(1, 4));
		}

		// One list for the round, worst first: the lines that cost players, ranked by how many each cost beyond a won fight of
		// this size (ally downs and deaths it brought, enemy kills missed: the Cost column); then how the ally pushes went
		// (shares and rates: not in players, so apart); then the lines that were fine, as icons. Allies downed and died are the
		// result, not a reason: they're in the answer above, not rows.
		void RoundTable(const RoundWhy& w)
		{
			std::vector<LineRow> cost, pushes, fine;
			for (int id = 0; id < L_Count; id++)
			{
				const LineRow r = RowOf(w, id);
				if (!r.Known) { continue; }
				if (r.Verdict == 0) { fine.push_back(r); }
				else if (kDefs[id].Part == S_EnemyDowned) { pushes.push_back(r); }
				else { cost.push_back(r); }
			}
			std::sort(cost.begin(), cost.end(), [](const LineRow& a, const LineRow& b) { return a.Excess > b.Excess; });
			std::sort(pushes.begin(), pushes.end(), [](const LineRow& a, const LineRow& b) { return a.Verdict != b.Verdict ? a.Verdict > b.Verdict : a.Excess > b.Excess; });
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float lh = ImGui::GetTextLineHeight();
			float labelW = ImGui::CalcTextSize("WHAT COST THE MOST").x;
			for (const Def& d : kDefs) { labelW = std::max(labelW, ImGui::CalcTextSize(d.Label).x); }
			const float numW = ImGui::CalcTextSize("This round").x + 8;
			if (!ImGui::BeginTable("why_round", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) { return; }
			ImGui::TableSetupColumn("##mark", ImGuiTableColumnFlags_WidthFixed, lh * 2.0f);
			ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelW + 8);
			ImGui::TableSetupColumn("This round", ImGuiTableColumnFlags_WidthFixed, numW);
			// won fights of this round's size: "Won 20v20"
			const std::string wonHead = std::string("Won ") + w.Band->Name, wonTip = std::string("A won ") + w.Band->Name + " fight";
			ImGui::TableSetupColumn(wonHead.c_str(), ImGuiTableColumnFlags_WidthFixed, std::max(numW, ImGui::CalcTextSize(wonHead.c_str()).x + 12));
			ImGui::TableSetupColumn("Cost", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Cost").x + 12);
			ImGui::TableSetupColumn("Who", ImGuiTableColumnFlags_WidthStretch);
			Headers({{"##mark", nullptr}, {"##label", nullptr}, {"This round", "This round's count or share"}, {wonHead.c_str(), wonTip.c_str()},
				{"Cost", "Players it cost, estimated"}, {"Who", "Who, most first"}});
			int rowId = 0;
			auto quietSelectable = [&](bool* aHov)
			{
				ImGui::PushID(rowId++);
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0x1d, 0x26, 0x36, 255));
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0x23, 0x2e, 0x42, 255));
				const bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap, ImVec2(0, lh));
				ImGui::PopStyleColor(2);
				*aHov = ImGui::IsItemHovered();
				ImGui::PopID();
				return clicked;
			};
			auto title = [&](const char* aText)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(1);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				SmallText(dl, ImVec2(p.x, p.y + 2), ImGui::GetColorU32(kMuted), aText);
				ImGui::Dummy(ImVec2(1, lh));
			};
			auto line = [&](const LineRow& r, bool aCost)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const ImVec2 q = ImGui::GetCursorScreenPos();
				bool rowHov = false;
				const bool rowClicked = quietSelectable(&rowHov);
				Mark(dl, ImVec2(q.x + lh * 0.3f, q.y + lh * 0.5f), lh * 0.24f, VerdictShape(r.Verdict), VerdictColour(r.Verdict));
				LineIcon(dl, ImVec2(q.x + lh * 0.8f, q.y), lh, r.Id);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(kDefs[r.Id].Label);
				NumCell(kDefs[r.Id].MoreIsWorse ? I(r.Value) : Format(r.Id, r.Value));
				NumCell(kDefs[r.Id].MoreIsWorse ? I(r.Expected) : Format(r.Id, r.Expected), &kMuted);
				NumCell(!aCost ? std::string() : r.Excess < 0.5 ? std::string("<1") : I(r.Excess));
				ImGui::TableNextColumn();
				const ImVec2 wp = ImGui::GetCursorScreenPos();
				const Whos who = WhoOf(w, r.Id);
				People(dl, r.Id, who);
				const bool whoHov = !who.empty() && ImGui::IsMouseHoveringRect(wp, ImVec2(wp.x + ImGui::GetContentRegionAvail().x, wp.y + lh));
				if (rowHov && !whoHov) { TipWrapped(Tip(w, r)); }
				if (rowClicked) { Open(w, r.Id); }
			};
			if (!cost.empty()) { title("WHAT COST THE MOST"); }
			for (const LineRow& r : cost) { line(r, true); }
			if (!pushes.empty()) { title("ALLY PUSHES"); }
			for (const LineRow& r : pushes) { line(r, false); }
			if (!fine.empty())
			{
				// the lines that were fine: an icon and a value each
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(1);
				{
					const ImVec2 p = ImGui::GetCursorScreenPos();
					SmallText(dl, ImVec2(p.x, p.y + 2), ImGui::GetColorU32(kMuted), "FINE");
					ImGui::Dummy(ImVec2(1, lh));
				}
				ImGui::TableSetColumnIndex(5); // the chips in the wide column
				float x = ImGui::GetCursorScreenPos().x;
				const float y = ImGui::GetCursorScreenPos().y;
				bool chipHov = false;
				for (const LineRow& r : fine)
				{
					const std::string id = "fine" + std::to_string(r.Id);
					if (Chip(id.c_str(), x, y, [id = r.Id](ImDrawList* d, ImVec2 q, float s) { LineIcon(d, q, s, id); }, kDefs[r.Id].MoreIsWorse ? I(r.Value) : Format(r.Id, r.Value),
						std::string(kDefs[r.Id].Label) + ": fine (" + wonHead + " " + (kDefs[r.Id].MoreIsWorse ? I(r.Expected) : Format(r.Id, r.Expected)) + ")", &chipHov)) { Open(w, r.Id); }
				}
				ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y));
				ImGui::Dummy(ImVec2(1, lh));
			}
			ImGui::EndTable();
		}

		// ---- tonight ------------------------------------------------------------------------------------------------------
		// A round's start on the clock ("21:59"): its log's name is its end
		std::string StartClock(const Fight& f)
		{
			if (f.Stamp.size() < 15) { return f.Stamp; }
			int secs = std::atoi(f.Stamp.substr(9, 2).c_str()) * 3600 + std::atoi(f.Stamp.substr(11, 2).c_str()) * 60 + std::atoi(f.Stamp.substr(13, 2).c_str());
			secs = ((secs - static_cast<int>(f.DurationMs / 1000)) % 86400 + 86400) % 86400;
			char b[8];
			std::snprintf(b, sizeof b, "%02d:%02d", secs / 3600, secs / 60 % 60);
			return b;
		}
		void NightView(const Ctx& c)
		{
			const auto& fights = *c.Fights;
			std::vector<int> rounds;
			for (int i = 0; i < static_cast<int>(fights.size()); i++)
			{
				if (fights[i] && fights[i]->DurationMs >= 30000 && fights[i]->EnemyDowns != fights[i]->SquadDowns) { rounds.push_back(i); }
			}
			if (rounds.size() < 2) { Answer("Fewer than two rounds with a result tonight."); return; }
			int won = 0;
			for (int i : rounds) { won += WhyOf(fights, i).Result > 0; }
			const int lost = static_cast<int>(rounds.size()) - won;
			// each line: the lost rounds' average a round, the won rounds', and the round it cost most in
			struct NightRow { int Id; double Lost = 0, Won = 0, Excess = 0; int LostN = 0, WonN = 0, Worst = -1; double WorstExcess = -1e9; int Verdicts = 0; };
			std::vector<NightRow> rows;
			for (int id = 0; id < L_Count; id++)
			{
				NightRow n{id};
				for (int i : rounds)
				{
					const RoundWhy& w = WhyOf(fights, i);
					const LineRow r = RowOf(w, id);
					if (!r.Known) { continue; }
					if (w.Result < 0) { n.Lost += r.Value; n.LostN++; n.Excess += r.Excess; n.Verdicts += r.Verdict > 0; }
					else { n.Won += r.Value; n.WonN++; }
					if (w.Result < 0 && r.Verdict > 0 && r.Excess > n.WorstExcess) { n.WorstExcess = r.Excess; n.Worst = i; }
				}
				if (n.LostN) { n.Lost /= n.LostN; }
				if (n.WonN) { n.Won /= n.WonN; }
				rows.push_back(n);
			}
			std::vector<NightRow> order = rows;
			std::stable_sort(order.begin(), order.end(), [](const NightRow& a, const NightRow& b) { return a.Verdicts != b.Verdicts ? a.Verdicts > b.Verdicts : a.Excess > b.Excess; });
			std::string answer = "Won " + std::to_string(won) + " of " + std::to_string(rounds.size());
			int named = 0;
			for (const NightRow& n : order)
			{
				if (named >= 2 || n.Verdicts == 0 || kDefs[n.Id].Part == S_EnemyDowned) { continue; }
				answer += (named ? " \xc2\xb7 " : ". Most lost rounds: ") + Low(kDefs[n.Id].Label);
				named++;
			}
			Answer(answer);
			ImGui::Dummy(ImVec2(1, 2));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float lh = ImGui::GetTextLineHeight();
			float labelW = ImGui::CalcTextSize("ENEMIES DOWNED").x;
			for (const Def& d : kDefs) { labelW = std::max(labelW, ImGui::CalcTextSize(d.Label).x); }
			const float numW = ImGui::CalcTextSize("Lost rounds").x + 8;
			if (!ImGui::BeginTable("why_night", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) { return; }
			ImGui::TableSetupColumn("##mark", ImGuiTableColumnFlags_WidthFixed, lh * 2.0f);
			ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelW + 8);
			ImGui::TableSetupColumn("Lost rounds", ImGuiTableColumnFlags_WidthFixed, numW);
			ImGui::TableSetupColumn("Won rounds", ImGuiTableColumnFlags_WidthFixed, numW);
			ImGui::TableSetupColumn("Worse in", ImGuiTableColumnFlags_WidthFixed, numW);
			ImGui::TableSetupColumn("Worst round", ImGuiTableColumnFlags_WidthStretch);
			Headers({{"##mark", nullptr}, {"##label", nullptr}, {"Lost rounds", "Average of a lost round"}, {"Won rounds", "Average of a won round"},
				{"Worse in", "Lost rounds past won fights"}, {"Worst round", "Click: open that round"}});
			int rowId = 0;
			// one list, the lines worse in the most lost rounds first; the result (downs, deaths) isn't a line
			for (const NightRow& n : order)
			{
				if (n.LostN == 0 && n.WonN == 0) { continue; }
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const ImVec2 q = ImGui::GetCursorScreenPos();
				ImGui::PushID(rowId++);
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0x1d, 0x26, 0x36, 255));
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0x23, 0x2e, 0x42, 255));
				const bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, lh));
				ImGui::PopStyleColor(2);
				const bool hov = ImGui::IsItemHovered();
				ImGui::PopID();
				const int v = lost == 0 ? 0 : n.Verdicts * 2 >= lost ? 2 : n.Verdicts > 0 ? 1 : 0;
				Mark(dl, ImVec2(q.x + lh * 0.3f, q.y + lh * 0.5f), lh * 0.24f, VerdictShape(v), VerdictColour(v));
				LineIcon(dl, ImVec2(q.x + lh * 0.8f, q.y), lh, n.Id);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(kDefs[n.Id].Label);
				auto fmt = [&](double x, int nRounds) { return nRounds == 0 ? std::string("-") : kDefs[n.Id].MoreIsWorse ? S1(x) : Format(n.Id, x); };
				NumCell(fmt(n.Lost, n.LostN));
				NumCell(fmt(n.Won, n.WonN), &kMuted);
				NumCell(std::to_string(n.Verdicts) + " of " + std::to_string(lost));
				ImGui::TableNextColumn();
				if (n.Worst >= 0) { ImGui::TextColored(kMuted, "%s  >", StartClock(*fights[n.Worst]).c_str()); }
				if (hov && n.Worst >= 0) { TipWrapped("Click: the round it cost most in"); }
				if (clicked && n.Worst >= 0) { S().Selected = n.Worst; S().StruggleNight = false; }
			}
			ImGui::EndTable();
		}
	}

	int StruggleTop(const Ctx& c, std::vector<StruggleWhy>& aOut)
	{
		aOut.clear();
		const Fight& f = *c.F;
		if (!c.Fights || f.DurationMs < 30000) { return 0; }
		const RoundWhy& w = WhyOf(*c.Fights, IndexOf(c));
		if (w.Result == 0) { return 0; }
		if (w.Result < 0)
		{
			for (const LineRow& r : Top(w, 2)) { aOut.push_back({r.Verdict, r.Id, kDefs[r.Id].Label, Short(w, r)}); }
		}
		else
		{
			// won: where it beat a won fight of its size by the most
			for (const LineRow& r : Best(w, 2)) { aOut.push_back({0, r.Id, kDefs[r.Id].Label, Short(w, r)}); }
		}
		return w.Result;
	}

	void StruggleIconAt(ImDrawList* dl, ImVec2 aPos, float aSize, int aFactor) { LineIcon(dl, aPos, aSize, aFactor); }

	bool StruggleDownHas(const Ctx& c, const Player& p, int32_t aMs, int aCause)
	{
		if (!c.Fights || aCause < 0 || aCause >= Causes::F_Count) { return false; }
		const RoundWhy& w = WhyOf(*c.Fights, IndexOf(c));
		const int idx = static_cast<int>(&p - c.F->Players.data());
		for (size_t i = 0; i < w.R.AllyDowns.size(); i++)
		{
			if (w.R.AllyDowns[i].Player == idx && w.R.AllyDowns[i].Ms == aMs) { return (w.S.Flags[i] >> aCause) & 1u; }
		}
		return false;
	}

	const char* StruggleCauseLabel(int aCause) { return aCause >= 0 && aCause < Causes::F_Count ? kDefs[L_Cc + aCause].Label : "Cause"; }

	void StruggleView(const Ctx& c)
	{
		State& s = S();
		const Fight& f = *c.F;
		ScopeSwitch("struggle", s.StruggleNight, "This round, or tonight's rounds");
		if (s.StruggleNight) { NightView(c); return; }
		if (!c.Fights) { return; }
		const RoundWhy& w = WhyOf(*c.Fights, IndexOf(c));
		if (w.Result == 0 || f.DurationMs < 30000)
		{
			Answer(w.Result == 0 ? "Even: both sides downed as many." : "Too short to tell (under 30 s).");
			return;
		}
		// the answer: the result, then what cost the most
		std::string answer = std::string(w.Result < 0 ? "Lost " : "Won ") + std::to_string(f.EnemyDowns) + " : " + std::to_string(f.SquadDowns);
		if (w.Result < 0)
		{
			const std::vector<LineRow> top = Top(w, 2);
			for (size_t i = 0; i < top.size(); i++) { answer += (i ? " \xc2\xb7 " : ". Cost the most: ") + Low(kDefs[top[i].Id].Label); }
			if (top.empty()) { answer += ". Nothing cost more than in won fights."; }
		}
		else
		{
			// won: what went better than in a won fight of this size (the list below still shows what cost players)
			const std::vector<LineRow> best = Best(w, 2);
			for (size_t i = 0; i < best.size(); i++) { answer += (i ? " \xc2\xb7 " : std::string(". Better than a won ") + w.Band->Name + ": ") + Low(kDefs[best[i].Id].Label); }
			if (best.empty()) { answer += std::string(". Nothing stood out against won ") + w.Band->Name + " fights."; }
		}
		Answer(answer);
		// the downs once the round was decided: counted apart
		if (w.S.AfterStopDowns > 0)
		{
			ImGui::TextColored(kMuted, "%s after allies stopped fighting at %s: not counted below.", Plural(w.S.AfterStopDowns, "down", "downs").c_str(),
				Duration(w.S.AllyStopMs).c_str());
			if (ImGui::IsItemHovered())
			{
				TipWrapped("From " + Duration(w.S.AllyStopMs) + " ally damage stayed under a third of its usual level to the end.\nThose " +
					std::to_string(w.S.AfterStopDowns) + " downs (" + std::to_string(w.S.AfterStopDeaths) + " died) came once the round was decided.");
			}
		}
		ImGui::Dummy(ImVec2(1, 2));
		Moments(w);
		RoundTable(w);
		ImGui::Dummy(ImVec2(1, 4));
	}
}
