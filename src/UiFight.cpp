// What led to a down (the cause line Deaths, You and the summary window use), and the revive order: who carries a
// revive skill, in which order they should use it, and the revive skills used in a round checked against it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "Ui.h"
#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		constexpr float kReviveWalk = 1800;

		const Player::Point* PosAt(const Player& p, int32_t aMs)
		{
			auto it = std::lower_bound(p.Pos.begin(), p.Pos.end(), aMs, [](const Player::Point& a, int32_t ms) { return a.Ms < ms; });
			if (it == p.Pos.end()) { return p.Pos.empty() ? nullptr : &p.Pos.back(); }
			if (it != p.Pos.begin() && aMs - (it - 1)->Ms < it->Ms - aMs) { --it; }
			return std::abs(it->Ms - aMs) <= 2000 ? &*it : nullptr;
		}

		// The squad's revive order: Tempests and Catalysts (Glyph of Renewal, the fastest), then Mesmers (Illusion of
		// Life), then Druids (Spirit of Nature) and Paragons (Battle Standard). The usual order (the user, 2026-09-24
		// and 25): Tempests and Catalysts first, then Mesmers by subgroup, then Druids and Paragons by subgroup, a Druid
		// and a Paragon in the same subgroup in either order. The player can set their own.
		constexpr int32_t kGlyphOfRenewal = 5573;
		bool IsGlyphOfRenewal(int32_t aSkill) { return aSkill == 5573 || (aSkill >= 5760 && aSkill <= 5763); } // + its attuned versions

		// Two ids for the same revive tool (Glyph of Renewal's attunements count as the glyph)
		bool SameTool(int32_t aSkill, int32_t aTool) { return aSkill == aTool || (IsGlyphOfRenewal(aSkill) && IsGlyphOfRenewal(aTool)); }

		bool OrderTool(const Player& p, int32_t aSkill)
		{
			return (IsGlyphOfRenewal(aSkill) && (p.Spec == "Tempest" || p.Spec == "Catalyst")) || (aSkill == 10244 && p.Profession == "Mesmer")
				|| (aSkill == 12569 && p.Spec == "Druid") || (aSkill == 14419 && p.Spec == "Paragon");
		}

		struct Turn { const Player* P; int Tier; }; // equal tiers: either may go first

#include "SkillSlots.inc"

		// A skill's slot (1 heal, 2 utility, 3 elite) and root (states of one slot share it); {0, 0} if unknown. By id,
		// else by name: logs carry other ids for some skills (Signet of Illusions 10246, the API 10247)
		std::pair<int, int32_t> SlotOf(int32_t aSkill, const std::string& aName)
		{
			static const auto tables = []
			{
				std::pair<std::map<int32_t, std::pair<int, int32_t>>, std::map<std::string, std::pair<int, int32_t>>> t;
				for (const SkillSlot& s : kSkillSlots) { t.first[s.Skill] = {s.Slot, s.Root}; t.second.emplace(s.Name, std::pair<int, int32_t>{s.Slot, s.Root}); }
				return t;
			}();
			if (auto it = tables.first.find(aSkill); it != tables.first.end()) { return it->second; }
			if (auto it = tables.second.find(aName); it != tables.second.end()) { return it->second; }
			return {0, 0};
		}

		// Utility tools (Illusion of Life, Glyph of Renewal): three other utility skills mean it isn't slotted; elite
		// tools (Spirit of Nature, Battle Standard): one other elite
		bool UtilityTool(int32_t aTool) { return aTool == 10244 || IsGlyphOfRenewal(aTool); }

		// Other skills cast from the revive tool's slot type
		void OtherSlots(const Fight& f, const Player& p, int32_t aTool, std::set<int32_t>& aRoots)
		{
			int slot = UtilityTool(aTool) ? 2 : 3;
			int32_t toolRoot = SlotOf(aTool, "").second;
			for (auto& [sk, r] : p.Skills)
			{
				if (SameTool(sk, aTool)) { continue; }
				auto it = f.SkillNames.find(sk);
				auto [s, root] = SlotOf(sk, it == f.SkillNames.end() ? std::string() : it->second);
				if (r.Casts > 0 && s == slot && root != toolRoot) { aRoots.insert(root); }
			}
		}

		// Did they carry their revive tool this round? Yes if they used it. No if the slot evidently held something
		// else: a Mesmer with three other utility skills (the user's rule: some run Veil or are DPS), a Druid or Paragon
		// with another elite; this round, or over tonight when they never used the tool tonight (instant skills leave
		// no cast, so one round rarely shows all three). Otherwise assumed yes.
		bool Carries(const Ctx& c, const Player& p, int32_t aTool)
		{
			for (auto& u : p.ReviveUses) { if (SameTool(u.Skill, aTool)) { return true; } }
			size_t enough = UtilityTool(aTool) ? 3 : 1;
			std::set<int32_t> roots;
			OtherSlots(*c.F, p, aTool, roots);
			if (roots.size() >= enough) { return false; }
			bool usedTonight = false;
			for (const auto& fp : *c.Fights)
			{
				for (const Player& q : fp->Players)
				{
					if (q.Account != p.Account || q.Spec != p.Spec) { continue; }
					for (auto& u : q.ReviveUses) { usedTonight |= SameTool(u.Skill, aTool); }
					OtherSlots(*fp, q, aTool, roots);
				}
			}
			return usedTonight || roots.size() < enough;
		}

		// The usual order: Tempests and Catalysts by subgroup, then Mesmers by subgroup, then Druids and Paragons by
		// subgroup (a subgroup's Druid and Paragon are one tier)
		std::vector<Turn> UsualOrder(const Ctx& c)
		{
			const Fight& f = *c.F;
			std::vector<const Player*> glyphs, mesmers, others;
			for (const Player& p : f.Players)
			{
				if ((p.Spec == "Tempest" || p.Spec == "Catalyst") && Carries(c, p, kGlyphOfRenewal)) { glyphs.push_back(&p); }
				else if (p.Profession == "Mesmer" && Carries(c, p, 10244)) { mesmers.push_back(&p); }
				else if ((p.Spec == "Druid" && Carries(c, p, 12569)) || (p.Spec == "Paragon" && Carries(c, p, 14419))) { others.push_back(&p); }
			}
			auto bySub = [](const Player* a, const Player* b) { return a->Subgroup != b->Subgroup ? a->Subgroup < b->Subgroup : (a->Spec != b->Spec ? a->Spec < b->Spec : a->Name < b->Name); };
			std::sort(glyphs.begin(), glyphs.end(), bySub);
			std::sort(mesmers.begin(), mesmers.end(), bySub);
			std::sort(others.begin(), others.end(), bySub);
			std::vector<Turn> usual;
			for (const Player* p : glyphs) { usual.push_back({p, static_cast<int>(usual.size())}); }
			for (const Player* p : mesmers) { usual.push_back({p, static_cast<int>(usual.size())}); }
			int tier = static_cast<int>(usual.size()) - 1, lastSub = -1;
			for (const Player* p : others) { if (p->Subgroup != lastSub) { tier++; lastSub = p->Subgroup; } usual.push_back({p, tier}); }
			return usual;
		}

		// This round's revive order: the Tempests, Catalysts, Mesmers, Druids and Paragons here who carried their revive tool
		std::vector<Turn> ReviveOrder(const Ctx& c)
		{
			// The usual order only changes with the rounds loaded (Carries looks over tonight): keep it, and the
			// round it points into, until then
			static FightPtr cachedRound, cachedLast;
			static size_t cachedCount = 0;
			static std::vector<Turn> cachedUsual;
			const FightPtr& round = (*c.Fights)[c.Index];
			if (round != cachedRound || c.Fights->back() != cachedLast || c.Fights->size() != cachedCount)
			{
				cachedRound = round;
				cachedLast = c.Fights->back();
				cachedCount = c.Fights->size();
				cachedUsual = UsualOrder(c);
			}
			const std::vector<Turn>& usual = cachedUsual;
			const auto& mine = S().ReviveOrder;
			if (mine.empty()) { return usual; }
			// Your order: the players in it first, in its order; anyone new after them, in the usual order
			std::vector<Turn> out;
			for (const std::string& acc : mine) { for (const Turn& t : usual) { if (t.P->Account == acc) { out.push_back({t.P, static_cast<int>(out.size())}); } } }
			for (const Turn& t : usual)
			{
				if (std::find(mine.begin(), mine.end(), t.P->Account) == mine.end()) { out.push_back({t.P, static_cast<int>(out.size())}); }
			}
			return out;
		}

	}

	// Revive skills in the order they went off, checked against the revive order
	void RevivesTable(const Ctx& c)
		{
			const Fight& f = *c.F;
			struct Row { const Player* P; Player::ReviveUse U; };
			std::vector<Row> rows;
			for (const Player& p : f.Players) { for (auto& u : p.ReviveUses) { if (u.Done) { rows.push_back({&p, u}); } } }
			std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.U.Ms < b.U.Ms; });
			std::vector<Turn> order = ReviveOrder(c);
			// Why someone whose turn it was didn't go: from the log around the moment
			auto reason = [&](const Player& y, int32_t at) -> std::string
			{
				for (auto& u : y.ReviveUses)
				{
					if (OrderTool(y, u.Skill) && !u.Done && u.Ms >= at - 4000 && u.Ms <= at + 3000)
					{
						return u.Stop == 9 ? "cast cut short, died" : u.Stop == 10 ? "cast cut short, downed" : u.Stop == 11 ? "cast cut short, CC'd"
							: u.Stop == 8 ? "cast interrupted" : "cast cancelled";
					}
				}
				for (auto& u : y.ReviveUses)
				{
					if (OrderTool(y, u.Skill) && u.Done && u.Ms > at && u.Ms <= at + 4000) { return "went " + Num((u.Ms - at) / 1000.0) + " s later"; }
					if (OrderTool(y, u.Skill) && u.Done && u.DownNear == 0 && std::abs(u.Ms - at) <= 4000) { return "cast with nobody down in reach"; }
				}
				for (int32_t ms : y.CcMs) { if (ms >= at - 3000 && ms <= at) { return "CC'd"; } }
				return "didn't cast";
			};
			auto tierOf = [&](const Player* p) { for (const Turn& t : order) { if (t.P == p) { return t.Tier; } } return -1; };
			auto downAt = [](const Player& p, int32_t ms) { for (const auto& s : p.DownSpans) { if (s.From <= ms && s.To >= ms) { return true; } } return false; };
			// Out of turn: someone earlier in the order, up and not yet used this time round, was skipped. When everyone
			// has used theirs, the order starts again.
			std::vector<std::string> verdict(rows.size());
			std::set<const Player*> used;
			int early = 0;
			for (size_t i = 0; i < rows.size(); i++)
			{
				const Row& r = rows[i];
				int tier = OrderTool(*r.P, r.U.Skill) ? tierOf(r.P) : -1;
				if (tier < 0) { verdict[i] = "-"; continue; }
				std::string skipped;
				int nSkipped = 0;
				for (const Turn& t : order)
				{
					if (t.Tier >= tier || used.count(t.P) || downAt(*t.P, r.U.Ms)) { continue; }
					// too far to walk over and cast it: not skipped (cast range 1200 plus a short walk)
					const Player::Point* a = PosAt(*t.P, r.U.Ms);
					const Player::Point* b = PosAt(*r.P, r.U.Ms);
					if (a && b && std::hypot(a->X - b->X, a->Y - b->Y) > kReviveWalk) { continue; }
					skipped += (skipped.empty() ? "" : ", ") + t.P->Name + ": " + reason(*t.P, r.U.Ms);
					nSkipped++;
				}
				verdict[i] = skipped.empty() ? "yes" : "no: skipped " + std::to_string(nSkipped) + " (" + skipped + ")";
				early += !skipped.empty();
				used.insert(r.P);
				if (used.size() >= order.size()) { used.clear(); }
			}

			ImGui::Spacing();
			if (rows.empty()) { ImGui::TextUnformatted("No revive skills were used this round."); }
			else
			{
				int wasted = static_cast<int>(std::count_if(rows.begin(), rows.end(), [](const Row& r) { return r.U.DownNear == 0; }));
				ImGui::Text("Revive skills in the order used: %d, %d with nobody down in reach, %d out of turn", static_cast<int>(rows.size()), wasted, early);
				ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
				if (ImGui::BeginTable("revives", 7, flags))
				{
					ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 45);
					ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 170);
					ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthFixed, 150);
					ImGui::TableSetupColumn("Down in reach", ImGuiTableColumnFlags_WidthFixed, 100);
					ImGui::TableSetupColumn("Got up", ImGuiTableColumnFlags_WidthFixed, 70);
					ImGui::TableSetupColumn("Turn", ImGuiTableColumnFlags_WidthFixed, 45);
					ImGui::TableSetupColumn("In turn", ImGuiTableColumnFlags_WidthStretch);
					Headers({{"Time", nullptr}, {"Player", nullptr}, {"Skill", nullptr}, {"Down in reach", "Allies downed within 1500"}, {"Got up", "Of those, up within 3 s"},
						{"Turn", "Place in the revive order"}, {"In turn", "Or who was skipped, why"}});
					for (size_t i = 0; i < rows.size(); i++)
					{
						const Row& r = rows[i];
						ImGui::TableNextRow();
						Cell(Duration(r.U.Ms));
						ImGui::TableNextColumn();
						SpecIcon(*r.P);
						ImGui::TextUnformatted((r.P->Name + (r.P->Pov ? " (you)" : "")).c_str());
						Cell(f.SkillNames.count(r.U.Skill) ? f.SkillNames.at(r.U.Skill) : std::to_string(r.U.Skill));
						NumCell(std::to_string(r.U.DownNear));
						NumCell(std::to_string(r.U.GotUp));
						int tier = OrderTool(*r.P, r.U.Skill) ? tierOf(r.P) : -1;
						NumCell(tier < 0 ? "-" : std::to_string(tier + 1));
						ImGui::TableNextColumn();
						ImGui::PushTextWrapPos(0.0f);
						ImGui::TextUnformatted(verdict[i].c_str());
						ImGui::PopTextWrapPos();
					}
					ImGui::EndTable();
				}
			}

			// The order itself, to check and change
			State& s = S();
			std::string head = std::string("Revive order: ") + (s.ReviveOrder.empty() ? "the usual (Tempests and Catalysts, then Mesmers by subgroup, then Druids and Paragons by subgroup)" : "yours") + "###reviveorder";
			if (ForcedOpen == kForceReviveOrder) { ImGui::SetNextItemOpen(true); } // render harness
			if (order.empty() || !ImGui::CollapsingHeader(head.c_str())) { return; }
			ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
			int move = -1, dir = 0;
			if (ImGui::BeginTable("reviveorder", 5, flags))
			{
				ImGui::TableSetupColumn("Turn", ImGuiTableColumnFlags_WidthFixed, 45);
				ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 200);
				ImGui::TableSetupColumn("Sg", ImGuiTableColumnFlags_WidthFixed, 30);
				ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthFixed, 130);
				ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 110);
				Headers({{"Turn", "Same number: either first"}, {"Player", nullptr}, {"Sg", "Subgroup"}, {"Skill", nullptr}, {"Move", nullptr}});
				for (size_t i = 0; i < order.size(); i++)
				{
					const Player& p = *order[i].P;
					ImGui::TableNextRow();
					NumCell(std::to_string(order[i].Tier + 1));
					ImGui::TableNextColumn();
					SpecIcon(p);
					ImGui::TextUnformatted((p.Name + (p.Pov ? " (you)" : "")).c_str());
					NumCell(std::to_string(p.Subgroup));
					Cell(p.Profession == "Mesmer" ? "Illusion of Life" : p.Spec == "Druid" ? "Spirit of Nature" : p.Spec == "Paragon" ? "Battle Standard" : "Glyph of Renewal");
					ImGui::TableNextColumn();
					ImGui::PushID(static_cast<int>(i));
					if (i > 0 && ImGui::SmallButton("Earlier")) { move = static_cast<int>(i); dir = -1; }
					if (i + 1 < order.size())
					{
						if (i > 0) { ImGui::SameLine(); }
						if (ImGui::SmallButton("Later")) { move = static_cast<int>(i); dir = 1; }
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (move >= 0)
			{
				// Your order from now on: this round's order with the move, then anyone from your old order not here
				std::vector<std::string> accs;
				for (const Turn& t : order) { accs.push_back(t.P->Account); }
				std::swap(accs[move], accs[move + dir]);
				for (const std::string& a : s.ReviveOrder) { if (std::find(accs.begin(), accs.end(), a) == accs.end()) { accs.push_back(a); } }
				s.ReviveOrder = accs;
				SaveSettings();
			}
			if (!s.ReviveOrder.empty() && ImGui::Button("Back to the usual order")) { s.ReviveOrder.clear(); SaveSettings(); }
		}

	namespace
	{
		DeathCause ComputeCause(const Fight& f, const Player& p, const Analysis::Span& s);
	}

	// A down's cause doesn't change once the round is loaded, and the Fight tab and the summary ask every frame
	DeathCause CauseOf(const Fight& f, const Player& p, const Analysis::Span& s)
	{
		static std::map<std::tuple<std::string, std::string, int32_t>, DeathCause> memo;
		auto key = std::make_tuple(f.Stamp, p.Account, s.From);
		auto it = memo.find(key);
		if (it == memo.end()) { it = memo.emplace(key, ComputeCause(f, p, s)).first; }
		return it->second;
	}

	namespace
	{
	DeathCause ComputeCause(const Fight& f, const Player& p, const Analysis::Span& s)
	{
		DeathCause d;
		const int32_t t = s.From, from = t - 6000;
		std::map<int32_t, double> by;
		for (const auto& h : p.HitsIn) { if (h.Ms >= from && h.Ms <= t) { d.Damage += h.Damage; by[h.Skill] += h.Damage; } }
		int32_t top = 0;
		for (auto& [sk, v] : by) { if (!top || v > by[top]) { top = sk; } }
		if (top) { d.TopSkill = f.SkillNames.count(top) ? f.SkillNames.at(top) : std::to_string(top); }
		int32_t full = -1; // last moment at 90%+ health
		for (auto& [ms, hp] : p.Hp) { if (ms > t) { break; } if (hp >= 9000) { full = ms; } }
		int cc = 0, bare = 0;
		for (int32_t ms : p.CcMs)
		{
			if (ms < from || ms > t) { continue; }
			cc++;
			bool had = false;
			for (auto& [a, b] : p.BoonOn[Analysis::kStability]) { if (a <= ms && b >= ms - 3000) { had = true; } }
			bare += !had;
		}
		int strips = 0, stab = 0;
		int32_t stabStripAt = -1; // the last time the enemy took stability off them
		for (const auto& st : p.StripsIn)
		{
			if (st.Ms < from || st.Ms > t) { continue; }
			strips++;
			if (st.Boon == Analysis::kStability) { stab++; stabStripAt = st.Ms; }
		}
		double healed = 0;
		for (auto& [ms, a] : p.HealsIn) { if (ms >= from && ms <= t) { healed += a; } }
		bool spike = false;
		for (int64_t sp : f.TheirSpikesMs) { if (t >= sp - 1000 && t <= sp + 4000) { spike = true; } }
		auto at = [](const Player& q, int32_t aMs) -> const Player::Point*
		{
			const Player::Point* best = nullptr;
			for (const auto& pt : q.Pos) { if (!best || std::abs(pt.Ms - aMs) < std::abs(best->Ms - aMs)) { best = &pt; } }
			return best && std::abs(best->Ms - aMs) <= 2000 ? best : nullptr;
		};
		double toTag = -1, moved = -1, ahead = 0;
		if (f.Commander >= 0 && &f.Players[f.Commander] != &p)
		{
			const auto* a = at(p, t);
			const auto* b = at(f.Players[f.Commander], t);
			if (a && b)
			{
				toTag = std::hypot(a->X - b->X, a->Y - b->Y);
				// Ahead of or behind the tag: distance to the enemy's centre (enemies within 2500 of the tag) for the
				// player against the commander
				double cx = 0, cy = 0;
				int n = 0;
				for (const auto& en : f.Enemies)
				{
					const Player::Point* best = nullptr;
					for (const auto& pt : en.Pos) { if (!best || std::abs(pt.Ms - t) < std::abs(best->Ms - t)) { best = &pt; } }
					if (!best || std::abs(best->Ms - t) > 2000 || std::hypot(best->X - b->X, best->Y - b->Y) > 2500) { continue; }
					cx += best->X; cy += best->Y; n++;
				}
				if (n >= 3) { ahead = std::hypot(b->X - cx / n, b->Y - cy / n) - std::hypot(a->X - cx / n, a->Y - cy / n); }
			}
		}
		if (const auto* a = at(p, t - 2000), *b = at(p, t); a && b && a != b) { moved = std::hypot(a->X - b->X, a->Y - b->Y); }

		// Each fact goes into Tags (one line, for Review and the summary) and into its group (the Fight tab's detail)
		static const char* kGroups[] = {"Damage", "Control", "Boons", "Support", "Position"};
		for (const char* g : kGroups) { d.Groups.push_back({g, {}}); }
		auto add = [&](int aGroup, const std::string& aText) { d.Tags.push_back(aText); d.Groups[aGroup].second.push_back(aText); };
		if (spike) { add(0, "in an enemy spike"); }
		if (full >= 0 && t - full <= 4000) { add(0, "90% to down in " + Num((t - full) / 1000.0) + " s"); }
		// Who hit them: how many enemies, and the spec that did the most
		std::map<int, double> byEnemy;
		for (const auto& h : p.HitsIn) { if (h.Ms >= from && h.Ms <= t && h.Enemy >= 0) { byEnemy[h.Enemy] += h.Damage; } }
		std::map<std::string, double> bySpec;
		for (auto& [en, v] : byEnemy) { bySpec[f.Enemies[en].Spec] += v; }
		std::string topSpec;
		for (auto& [sp, v] : bySpec) { if (topSpec.empty() || v > bySpec[topSpec]) { topSpec = sp; } }
		add(0, Num(d.Damage) + " damage in 6 s" + (d.TopSkill.empty() ? "" : ", most from " + d.TopSkill));
		if (!byEnemy.empty())
		{
			add(0, "hit by " + std::to_string(byEnemy.size()) + (byEnemy.size() == 1 ? " enemy" : " enemies") + (topSpec.empty() ? "" : ", most by " + topSpec + "s"));
		}
		if (cc) { add(1, "CC'd " + std::to_string(cc) + "x" + (bare ? " (" + std::to_string(bare) + " with no stability just before)" : "")); }
		bool pulled = false;
		for (int32_t ms : p.CcMs)
		{
			if (ms < from || ms > t) { continue; }
			for (int32_t tp : p.TeleportMs) { pulled |= tp >= ms - 100 && tp <= ms + 1000; }
		}
		if (pulled) { add(1, "pulled (moved by a teleport right after CC)"); }
		else if (moved >= 500 && cc) { add(1, "moved " + Num(moved) + " by CC (pulled or knocked)"); }
		if (strips) { add(2, std::to_string(strips) + (strips == 1 ? " boon" : " boons") + " stripped" + (stab ? ", stability among them, last " + Num((t - stabStripAt) / 1000.0) + " s before the down" : "")); }
		add(3, healed > 0 ? "healed " + Num(healed) : "no healing received");
		std::string on;
		for (int b : {Analysis::kStability, 7, 4, 10})
		{
			for (auto& [a, e] : p.BoonOn[b]) { if (a <= t - 200 && e >= t - 200) { on += (on.empty() ? "" : ", ") + std::string(Analysis::kBoonNames[b]); break; } }
		}
		add(2, on.empty() ? "at the down: no stability, aegis, protection or resistance" : "at the down: had " + on);
		// The subgroup's stability casts around the down: how long before, and one soon after (it was there to use)
		int32_t before = -1, after = -1;
		std::string beforeWho, afterWho;
		for (const Player& q : f.Players)
		{
			if (q.Subgroup != p.Subgroup) { continue; }
			for (auto& [sk, r] : q.Skills)
			{
				if (sk == 0 || r.BoonGroupS[Analysis::kStability] <= 0) { continue; }
				for (int32_t ms : r.CastMs)
				{
					if (ms <= t && ms > before) { before = ms; beforeWho = q.Name; }
					if (ms > t && ms <= t + 5000 && (after < 0 || ms < after)) { after = ms; afterWho = q.Name; }
				}
			}
		}
		if (before >= 0) { add(3, "the subgroup's last stability cast was " + Num((t - before) / 1000.0) + " s before the down (" + beforeWho + ")"); }
		else { add(3, "nobody in the subgroup cast stability this round before the down"); }
		if (after >= 0)
		{
			// did it reach them: stability on them starting within 1.5 s of the cast
			bool reached = false;
			for (auto& [a, e] : p.BoonOn[Analysis::kStability]) { reached |= a >= after - 100 && a <= after + 1500; }
			add(3, "next stability cast " + Num((after - t) / 1000.0) + " s after the down (" + afterWho + ")" + (reached ? ", it reached them" : ", it didn't reach them"));
		}
		if (toTag >= 0)
		{
			std::string where = ahead >= 300 ? ", " + Num(ahead) + " closer to the enemy (ahead)" : ahead <= -300 ? ", " + Num(-ahead) + " further from the enemy (behind)" : "";
			add(4, Num(toTag) + " from the commander" + (toTag > 1200 ? " (far)" : "") + where);
		}
		for (size_t i = 0; i < d.Tags.size(); i++) { d.Line += (i ? "; " : "") + d.Tags[i]; }
		// The short version: the damage, then only what stands out
		std::vector<std::string> brief;
		if (spike) { brief.push_back("enemy spike"); }
		brief.push_back(Num(d.Damage) + " damage" + (byEnemy.empty() ? "" : " from " + std::to_string(byEnemy.size()) + (byEnemy.size() == 1 ? " enemy" : " enemies")));
		if (bare) { brief.push_back("CC'd with no stability"); }
		if (pulled) { brief.push_back("pulled"); }
		if (stab) { brief.push_back("stability stripped"); }
		if (healed <= 0) { brief.push_back("no healing"); }
		if (toTag > 1200) { brief.push_back("far from the commander"); }
		for (size_t i = 0; i < brief.size(); i++) { d.Short += (i ? ", " : "") + brief[i]; }
		return d;
	}
	}
}
