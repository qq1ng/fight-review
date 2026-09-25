#include "Ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "Session.h"
#include "UiCommon.h"

namespace Ui
{
	bool ShowWindow = false;
	int ForcedTab = -1;
	int ForcedMetric = -1;
	int ForcedOpen = -1;
	std::string ForcedYou;
	bool ShowMini = false;

	namespace
	{
		std::filesystem::path s_SettingsFile; // empty: not saved (the render harness)
	}

	// key=value lines; revive_order is a comma-separated list of accounts
	void LoadSettings(const std::filesystem::path& aFile)
	{
		s_SettingsFile = aFile;
		std::ifstream in(aFile);
		std::string line;
		while (std::getline(in, line))
		{
			size_t eq = line.find('=');
			if (eq == std::string::npos) { continue; }
			std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			if (key == "summary") { ShowMini = value == "1"; }
			else if (key == "revive_order")
			{
				auto& order = S().ReviveOrder;
				order.clear();
				for (size_t a = 0; a < value.size();)
				{
					size_t b = value.find(',', a);
					if (b == std::string::npos) { b = value.size(); }
					if (b > a) { order.push_back(value.substr(a, b - a)); }
					a = b + 1;
				}
			}
		}
	}

	void SaveSettings()
	{
		if (s_SettingsFile.empty()) { return; }
		std::ofstream out(s_SettingsFile, std::ios::trunc);
		out << "summary=" << (ShowMini ? 1 : 0) << '\n';
		out << "revive_order=";
		const auto& order = S().ReviveOrder;
		for (size_t i = 0; i < order.size(); i++) { out << (i ? "," : "") << order[i]; }
		out << '\n';
	}

	namespace
	{
		// Mouse wheel over our window, when ImGui doesn't get it by itself (see OnMouseWheel)
		std::atomic<bool> s_Hovered{false};
		std::atomic<int>  s_PendingWheel{0};
		ImGuiWindow*      s_Window = nullptr;

		bool IsOurs(ImGuiWindow* aWindow)
		{
			for (ImGuiWindow* w = aWindow; w; w = w->ParentWindow) { if (w == s_Window) { return true; } }
			return false;
		}

		// Scrolls what's under the mouse with a wheel turn ImGui didn't see itself, the way ImGui would: the
		// hovered window, or the nearest parent that can scroll.
		void ApplyWheel()
		{
			int pending = s_PendingWheel.exchange(0);
			ImGuiContext& g = *ImGui::GetCurrentContext();
			if (pending == 0 || g.IO.MouseWheel != 0.0f || !s_Window) { return; }
			ImGuiWindow* w = g.HoveredWindow;
			if (!w || !IsOurs(w)) { return; }
			while ((w->Flags & ImGuiWindowFlags_ChildWindow) && w->ScrollMax.y == 0.0f && w->ParentWindow) { w = w->ParentWindow; }
			float step = ImFloor(ImMin(5 * w->CalcFontSize(), w->InnerRect.GetHeight() * 0.67f));
			ImGui::SetScrollY(w, w->Scroll.y - (pending / 120.0f) * step);
		}

		// A round's start (the log's name is its end), length, players each side, downs each side and who won them
		struct RoundFacts { std::string Start, Length, Players, Downs, Result; ImU32 Colour = 0; };
		RoundFacts Facts(const Fight& x)
		{
			RoundFacts r;
			int end = 0;
			if (x.Stamp.size() >= 15) { end = std::atoi(x.Stamp.substr(9, 2).c_str()) * 3600 + std::atoi(x.Stamp.substr(11, 2).c_str()) * 60 + std::atoi(x.Stamp.substr(13, 2).c_str()); }
			int start = ((end - static_cast<int>(x.DurationMs / 1000)) % 86400 + 86400) % 86400;
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%02d:%02d", start / 3600, start / 60 % 60);
			r.Start = buf;
			r.Length = Duration(x.DurationMs);
			r.Players = std::to_string(x.SquadCount) + " v " + std::to_string(x.EnemyCount);
			r.Downs = std::to_string(x.EnemyDowns) + " : " + std::to_string(x.SquadDowns);
			r.Result = x.EnemyDowns > x.SquadDowns ? "won" : x.EnemyDowns < x.SquadDowns ? "lost" : "even";
			r.Colour = x.EnemyDowns > x.SquadDowns ? kYou : x.EnemyDowns < x.SquadDowns ? kEnemy : kPeer;
			return r;
		}

		// One round as a line: a coloured bar (won, lost, even: the word says it too), then its facts in columns
		void RoundLine(ImDrawList* dl, ImVec2 p, const RoundFacts& r, bool aHeader = false)
		{
			float lh = ImGui::GetTextLineHeight();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			if (!aHeader) { dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + 5, p.y + lh - 1), r.Colour); }
			const float cols[] = {14, 70, 122, 200, 262};
			const std::string texts[] = {r.Start, r.Length, r.Players, r.Downs, r.Result};
			const char* heads[] = {"Start", "Length", "Players", "Downed", "Result"};
			for (int i = 0; i < 5; i++)
			{
				const char* t = aHeader ? heads[i] : texts[i].c_str();
				dl->AddText(ImVec2(p.x + cols[i], p.y), aHeader || i == 1 || i == 2 ? muted : ink, t);
			}
		}

		// Previous and next buttons, greyed at the ends
		bool StepButton(const char* aId, ImGuiDir aDir, bool aEnabled, const char* aTip)
		{
			if (!aEnabled) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f); }
			bool clicked = ImGui::ArrowButton(aId, aDir) && aEnabled;
			if (!aEnabled) { ImGui::PopStyleVar(); }
			if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", aTip); }
			return clicked;
		}

		// The round picker (by start and length, the result coloured and in words), This round / Tonight: every tab
		void HeaderRow(const Session::Snapshot& aSnap, int aIndex)
		{
			State& s = S();
			int count = static_cast<int>(aSnap.Fights.size());
			float lh = ImGui::GetTextLineHeight();
			if (StepButton("##earlier", ImGuiDir_Left, aIndex > 0, "Earlier round")) { s.Selected = aIndex - 1; s.OpenFix = -1; }
			ImGui::SameLine(0, 4);
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 at = ImGui::GetCursorScreenPos();
			const float width = 330;
			ImGui::SetNextItemWidth(width);
			bool open = ImGui::BeginCombo("##round", "", ImGuiComboFlags_HeightLarge);
			{
				// the picked round over the empty preview
				RoundFacts r = Facts(*aSnap.Fights[aIndex]);
				ImVec2 p(at.x + ImGui::GetStyle().FramePadding.x, at.y + ImGui::GetStyle().FramePadding.y);
				dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + 5, p.y + lh - 1), r.Colour);
				std::string text = r.Start + "  " + r.Length + "  " + r.Players + "  " + r.Result + ", " + r.Downs;
				dl->PushClipRect(at, ImVec2(at.x + width - ImGui::GetFrameHeight(), at.y + ImGui::GetFrameHeight()), true);
				dl->AddText(ImVec2(p.x + 12, p.y), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
				dl->PopClipRect();
			}
			if (open)
			{
				if (ImGui::Selectable("Follow the latest round", s.Selected < 0)) { s.Selected = -1; s.OpenFix = -1; }
				ImDrawList* pd = ImGui::GetWindowDrawList();
				RoundLine(pd, ImGui::GetCursorScreenPos(), RoundFacts{}, true);
				ImGui::Dummy(ImVec2(320, lh));
				for (int i = count - 1; i >= 0; i--)
				{
					ImGui::PushID(i);
					ImVec2 p = ImGui::GetCursorScreenPos();
					if (ImGui::Selectable("##r", i == aIndex && s.Selected >= 0, 0, ImVec2(320, lh))) { s.Selected = i == count - 1 ? -1 : i; s.OpenFix = -1; }
					RoundLine(pd, p, Facts(*aSnap.Fights[i]));
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			else if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Downed: theirs : ours"); }
			ImGui::SameLine(0, 4);
			if (StepButton("##later", ImGuiDir_Right, aIndex < count - 1, "Later round")) { s.Selected = aIndex + 1 >= count - 1 ? -1 : aIndex + 1; s.OpenFix = -1; }
			ImGui::SameLine(0, 10);
			ImGui::AlignTextToFramePadding();
			std::string where = s.Selected < 0 ? "latest of " + std::to_string(count) : "round " + std::to_string(aIndex + 1) + " of " + std::to_string(count);
			ImGui::TextColored(kMuted, "%s", where.c_str());
			// This round / Tonight, as one control at the right
			float segW = ImGui::CalcTextSize("This round").x + ImGui::CalcTextSize("Tonight").x + ImGui::GetStyle().FramePadding.x * 4 + 2;
			ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowContentRegionMax().x - segW));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
			auto seg = [](const char* aLabel, bool aOn)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(aOn ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
				bool clicked = ImGui::Button(aLabel);
				ImGui::PopStyleColor();
				return clicked;
			};
			if (seg("This round", !s.AllRounds)) { s.AllRounds = false; }
			ImGui::SameLine();
			if (seg("Tonight", s.AllRounds)) { s.AllRounds = true; }
			if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Every round loaded, added up"); }
			ImGui::PopStyleVar();
		}
	}

	bool OnMouseWheel(int aDelta)
	{
		if (!ShowWindow || !s_Hovered.load()) { return false; }
		s_PendingWheel.fetch_add(aDelta);
		return true;
	}

	// The latest round in a few lines, always on screen if the player wants it; a click opens the full review
	void RenderMini()
	{
		if (!ShowMini) { return; }
		Session::Snapshot snap = Session::Get();
		ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
		bool shown = ImGui::Begin("Fight Review summary", &ShowMini, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoCollapse);
		if (!ShowMini) { SaveSettings(); } // closed with its X
		if (!shown)
		{
			ImGui::End();
			return;
		}
		if (snap.Fights.empty()) { ImGui::TextColored(kMuted, "No rounds yet."); ImGui::End(); return; }
		int last = static_cast<int>(snap.Fights.size()) - 1;
		const Fight& f = *snap.Fights[last];
		{
			RoundFacts r = Facts(f);
			ImGui::TextColored(kMuted, "%s  %s  %s  %s, %s", r.Start.c_str(), r.Length.c_str(), r.Players.c_str(), r.Result.c_str(), r.Downs.c_str());
		}
		const Ctx& c = CachedCtx(snap.Fights, last, false);
		for (const std::string& line : SummaryLines(c)) { ImGui::TextUnformatted(line.c_str()); }
		if (c.MeRaw)
		{
			for (const Analysis::Span& sp : c.MeRaw->DownSpans)
			{
				if (sp.Dead) { continue; }
				ImGui::TextUnformatted(("Downed at " + Duration(sp.From) + ": " + CauseOf(f, *c.MeRaw, sp).Short).c_str());
			}
		}
		if (ImGui::SmallButton("Open the review")) { ShowWindow = true; S().Selected = -1; S().SwitchTo = T_You; }
		ImGui::End();
	}

	namespace
	{
		double s_FrameMs = 0, s_FrameAvg = 0, s_FrameWorst = 0; // what drawing the addon cost, for the options page
		int s_FrameCount = 0;
		void RenderAll();
	}

	// Drawing timed: the options page shows what the addon costs per frame (the user, 2026-09-25: low frames in a big fight)
	void Render()
	{
		auto t0 = std::chrono::steady_clock::now();
		RenderAll();
		s_FrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		s_FrameAvg = s_FrameCount++ ? s_FrameAvg * 0.98 + s_FrameMs * 0.02 : s_FrameMs;
		s_FrameWorst = std::max(s_FrameWorst * 0.999, s_FrameMs);
	}

	namespace
	{
	void RenderAll()
	{
		RenderMini();
		if (!ShowWindow) { s_Hovered = false; return; }
		ApplyWheel();
		ImGui::SetNextWindowSize(ImVec2(980, 760), ImGuiCond_FirstUseEver);
		bool open = ImGui::Begin(kWindowName, &ShowWindow);
		s_Window = ImGui::GetCurrentWindow();
		{
			ImGuiContext& g = *ImGui::GetCurrentContext();
			s_Hovered = g.HoveredWindow && IsOurs(g.HoveredWindow);
		}
		if (!open) { ImGui::End(); return; }

		Session::Snapshot snap = Session::Get();
		if (snap.Fights.empty())
		{
			ImGui::TextWrapped("No fights yet. Logs appear here a few seconds after ArcDPS saves them.");
			ImGui::TextColored(kMuted, "Watching %s", snap.Folder.string().c_str());
			if (!snap.Status.empty()) { ImGui::TextColored(kMuted, "%s", snap.Status.c_str()); }
			ImGui::End();
			return;
		}
		State& s = S();
		if (ForcedMetric >= 0) { s.Metric = ForcedMetric; }
		if (ForcedOpen >= 0) { s.OpenFix = ForcedOpen; }
		int count = static_cast<int>(snap.Fights.size());
		int index = s.Selected < 0 || s.Selected >= count ? count - 1 : s.Selected;
		const Ctx& c = CachedCtx(snap.Fights, index, s.AllRounds, ForcedYou);

		HeaderRow(snap, index);
		if (!snap.Status.empty()) { ImGui::TextColored(kMuted, "%s", snap.Status.c_str()); }

		int want = ForcedTab >= 0 ? ForcedTab : s.SwitchTo;
		s.SwitchTo = -1;
		auto tab = [want](const char* aName, int aIndex)
		{
			return ImGui::BeginTabItem(aName, nullptr, want == aIndex ? ImGuiTabItemFlags_SetSelected : 0);
		};
		if (ImGui::BeginTabBar("tabs"))
		{
			if (tab("You", T_You)) { ReviewTab(c); ImGui::EndTabItem(); }
			if (tab("Deaths", T_Deaths)) { DeathsTab(CachedCtx(snap.Fights, index, false, ForcedYou)); ImGui::EndTabItem(); }
			if (tab("Compare", T_Compare))
			{
				if (s.CompareLeft.empty() && s.CompareRight.empty()) { CompareTab(c); }
				else { CompareTab(CachedCtx(snap.Fights, index, s.AllRounds, s.CompareLeft, s.CompareRight)); }
				ImGui::EndTabItem();
			}
			if (tab("Round", T_Round)) { RoundTab(CachedCtx(snap.Fights, index, false, ForcedYou)); ImGui::EndTabItem(); }
			if (tab("Squad", T_Squad)) { SquadTab(c); ImGui::EndTabItem(); }
			if (tab("Tonight", T_Tonight))
			{
				// the night doesn't depend on the round picked: a round you took no part in falls back to your latest one
				int night = index;
				if (snap.Fights[night]->Pov < 0) { for (int i = count - 1; i >= 0; i--) { if (snap.Fights[i]->Pov >= 0) { night = i; break; } } }
				TonightTab(CachedCtx(snap.Fights, night, true, ForcedYou));
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();
	}

	}

	void Options()
	{
		Session::Snapshot snap = Session::Get();
		ImGui::Text("Log folder: %s", snap.Folder.string().c_str());
		ImGui::Text("Rounds loaded: %d", static_cast<int>(snap.Fights.size()));
		ImGui::Checkbox("Show the Fight Review window", &ShowWindow);
		if (ImGui::Checkbox("Show the small summary window", &ShowMini)) { SaveSettings(); }
		ImGui::Text("Drawing the windows costs %.2f ms a frame (average %.2f, recent worst %.1f)", s_FrameMs, s_FrameAvg, s_FrameWorst);
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("At 60 fps a frame has 16.7 ms"); }
	}
}
