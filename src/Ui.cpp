#include "Ui.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
	bool ShowMini = true; // on by default (the v6 design); the settings file keeps the player's choice

	namespace
	{
		// The summary window's style (right-click > Style), saved in settings.txt. Width or height 0: fit the text.
		struct MiniStyle { bool Title = true, Background = true; float Alpha = 1.0f, Width = 0, Height = 0; };
		MiniStyle s_Mini;
		bool s_MiniDirty = false; // changed in the menu, saved when the drag ends
		bool s_Gameplay = true, s_MapOpen = false;
	}

	void SetGameState(bool aGameplay, bool aMapOpen) { s_Gameplay = aGameplay; s_MapOpen = aMapOpen; }

	namespace
	{
		// "0.6" whatever the C locale says (another addon may set one that writes 0,6)
		float Number(const std::string& aText, float aDefault)
		{
			float v = aDefault;
			auto [end, err] = std::from_chars(aText.data(), aText.data() + aText.size(), v);
			return err == std::errc() ? v : aDefault;
		}
	}

	namespace
	{
		std::filesystem::path s_SettingsFile; // empty: not saved (the render harness)
		std::filesystem::path s_ArcdpsIni;    // empty: unknown (the render harness)

		// No rounds yet: what stands in the way, from arcdps.ini (read once a second at most)
		void FirstRun(const Session::Snapshot& aSnap)
		{
			static double checked = -10;
			static bool iniFound = false, wvw = true;
			static int minimum = -1;
			if (ImGui::GetTime() - checked > 1.0)
			{
				checked = ImGui::GetTime();
				iniFound = false; wvw = true; minimum = -1;
				std::ifstream in(s_ArcdpsIni);
				std::string line;
				while (!s_ArcdpsIni.empty() && std::getline(in, line))
				{
					iniFound = true;
					while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
					if (line.rfind("boss_encounter_savewvw=", 0) == 0) { wvw = line.substr(23) == "1"; }
					else if (line.rfind("minimum_log_duration=", 0) == 0) { minimum = std::atoi(line.c_str() + 21); }
				}
			}
			const ImVec4 bad(0xd9 / 255.0f, 0x59 / 255.0f, 0x26 / 255.0f, 1.0f), good(0x39 / 255.0f, 0x87 / 255.0f, 0xe5 / 255.0f, 1.0f);
			auto row = [](bool aOk, const ImVec4& aCol, const std::string& aText)
			{
				ImGui::TextColored(aCol, "%s", aOk ? "ok" : "no");
				ImGui::SameLine(ImGui::GetTextLineHeight() * 2.5f);
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(aText.c_str());
				ImGui::PopTextWrapPos();
			};
			std::error_code ec;
			bool folder = std::filesystem::exists(aSnap.Folder, ec);
			if (iniFound && !wvw) { ImGui::TextUnformatted("No WvW rounds yet: ArcDPS isn't saving WvW logs."); }
			else { ImGui::TextUnformatted("No fights yet. A round shows here a few seconds after ArcDPS saves its log; nothing is shown during a fight."); }
			ImGui::Spacing();
			if (!s_ArcdpsIni.empty())
			{
				if (!iniFound) { row(false, bad, "ArcDPS's settings (arcdps.ini) weren't found: is ArcDPS installed?"); }
				else if (wvw) { row(true, good, "ArcDPS saves WvW logs."); }
				else { row(false, bad, "WvW logs are off. Open ArcDPS's options (Alt+Shift+T by default) and turn on saving logs in WvW."); }
				if (iniFound && minimum > 30) { row(false, bad, "ArcDPS skips fights shorter than " + std::to_string(minimum) + " s (its minimum log length): short skirmishes won't show."); }
				else if (iniFound && minimum >= 0) { row(true, good, "Fights of " + std::to_string(minimum) + " s or more are saved."); }
			}
			row(folder, folder ? good : bad, (folder ? "Log folder: " : "Log folder not found yet (ArcDPS creates it with the first log): ") + Session::PathText(aSnap.Folder));
			if (!aSnap.Status.empty()) { ImGui::TextColored(kMuted, "%s", aSnap.Status.c_str()); }
		}
	}

	void SetArcdpsIni(const std::filesystem::path& aFile) { s_ArcdpsIni = aFile; }

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
			else if (key == "summary_title") { s_Mini.Title = value == "1"; }
			else if (key == "summary_background") { s_Mini.Background = value == "1"; }
			else if (key == "summary_alpha") { s_Mini.Alpha = std::clamp(Number(value, 1.0f), 0.0f, 1.0f); }
			else if (key == "summary_width") { s_Mini.Width = std::max(0.0f, Number(value, 0.0f)); }
			else if (key == "summary_height") { s_Mini.Height = std::max(0.0f, Number(value, 0.0f)); }
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
		out << "summary_title=" << (s_Mini.Title ? 1 : 0) << '\n';
		out << "summary_background=" << (s_Mini.Background ? 1 : 0) << '\n';
		out << "summary_alpha=" << s_Mini.Alpha << '\n';
		out << "summary_width=" << s_Mini.Width << '\n';
		out << "summary_height=" << s_Mini.Height << '\n';
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

		// The round picker (by start and length, the result coloured and in words): every tab
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
		}
	}

	bool OnMouseWheel(int aDelta)
	{
		if (!ShowWindow || !s_Hovered.load()) { return false; }
		s_PendingWheel.fetch_add(aDelta);
		return true;
	}

	namespace
	{
		// Right-click > Style, as on ArcDPS's windows: title bar, background and its opacity, width and height
		void MiniStyleMenu()
		{
			if (!ImGui::BeginPopupContextWindow("summary_menu")) { return; }
			if (ImGui::BeginMenu("Style"))
			{
				bool changed = false;
				changed |= ImGui::Checkbox("Title bar", &s_Mini.Title);
				changed |= ImGui::Checkbox("Background", &s_Mini.Background);
				ImGui::SetNextItemWidth(140);
				changed |= ImGui::SliderFloat("Background opacity", &s_Mini.Alpha, 0.0f, 1.0f, "%.2f");
				ImGui::SetNextItemWidth(140);
				changed |= ImGui::DragFloat("Width", &s_Mini.Width, 1.0f, 0.0f, 2000.0f, s_Mini.Width > 0 ? "%.0f" : "fit");
				if (ImGui::IsItemHovered()) { ImGui::SetTooltip("0: as wide as the text; set, the text wraps to it"); }
				ImGui::SetNextItemWidth(140);
				changed |= ImGui::DragFloat("Height", &s_Mini.Height, 1.0f, 0.0f, 2000.0f, s_Mini.Height > 0 ? "%.0f" : "fit");
				if (ImGui::IsItemHovered()) { ImGui::SetTooltip("0: as tall as the text"); }
				if (changed)
				{
					s_Mini.Width = std::max(0.0f, s_Mini.Width);
					s_Mini.Height = std::max(0.0f, s_Mini.Height);
					s_MiniDirty = true;
				}
				ImGui::EndMenu();
			}
			// with the title bar off there is no X: the way to close it (the options page brings it back)
			if (ImGui::MenuItem("Hide")) { ShowMini = false; SaveSettings(); }
			ImGui::EndPopup();
		}
	}

	// The latest round in a few lines, always on screen if the player wants it; a click opens the full review
	void RenderMini()
	{
		if (!ShowMini || s_MapOpen) { return; }
		Session::Snapshot snap = Session::Get();
		SetDataVersion(snap.Version);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
		if (!s_Mini.Title) { flags |= ImGuiWindowFlags_NoTitleBar; }
		if (!s_Mini.Background) { flags |= ImGuiWindowFlags_NoBackground; }
		// width and height 0 fit the text; a set one is fixed (the text wraps to the width, a height cuts it)
		if (s_Mini.Width > 0 && s_Mini.Height > 0) { ImGui::SetNextWindowSize(ImVec2(s_Mini.Width, s_Mini.Height), ImGuiCond_Always); flags |= ImGuiWindowFlags_NoScrollbar; }
		else
		{
			flags |= ImGuiWindowFlags_AlwaysAutoResize;
			ImVec2 least(s_Mini.Width, s_Mini.Height), most(s_Mini.Width > 0 ? s_Mini.Width : FLT_MAX, s_Mini.Height > 0 ? s_Mini.Height : FLT_MAX);
			ImGui::SetNextWindowSizeConstraints(least, most);
		}
		ImGui::SetNextWindowBgAlpha(s_Mini.Alpha);
		bool shown = ImGui::Begin("Fight Review summary", s_Mini.Title ? &ShowMini : nullptr, flags);
		if (!ShowMini) { SaveSettings(); } // closed with its X
		MiniStyleMenu();
		if (s_MiniDirty && !ImGui::IsAnyItemActive()) { SaveSettings(); s_MiniDirty = false; }
		if (!shown)
		{
			ImGui::End();
			return;
		}
		if (s_Mini.Width > 0) { ImGui::PushTextWrapPos(0.0f); }
		if (snap.Fights.empty())
		{
			ImGui::TextColored(kMuted, "No rounds yet.");
			if (s_Mini.Width > 0) { ImGui::PopTextWrapPos(); }
			ImGui::End();
			return;
		}
		int last = static_cast<int>(snap.Fights.size()) - 1;
		const Fight& f = *snap.Fights[last];
		{
			RoundFacts r = Facts(f);
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::TextUnformatted((r.Start + "  " + r.Length + "  " + r.Players + "  " + r.Result + ", " + r.Downs).c_str());
			ImGui::PopStyleColor();
		}
		const Ctx& c = CachedCtx(snap.Fights, last, false);
		for (const std::string& line : SummaryWindowLines(c)) { ImGui::TextUnformatted(line.c_str()); }
		if (ImGui::SmallButton("Open the summary")) { ShowWindow = true; S().Selected = -1; S().SwitchTo = T_Summary; }
		if (s_Mini.Width > 0) { ImGui::PopTextWrapPos(); }
		ImGui::End();
	}

	namespace
	{
		double s_FrameMs = 0, s_FrameAvg = 0, s_FrameWorst = 0; // what drawing the addon cost, for the options page
		int s_FrameCount = 0;
		void RenderAll();
	}

	// Drawing timed: the options page shows what the addon costs per frame (the user, 2026-09-25: low frames in a big fight)
	// An exception while drawing would leave our windows open (Begin without End) in Nexus' frame, which ImGui doesn't
	// check in a release build: close what we opened, then let it go on to be logged
	void Render()
	{
		auto t0 = std::chrono::steady_clock::now();
		ImGuiContext& g = *ImGui::GetCurrentContext();
		const int depth = g.CurrentWindowStack.Size;
		const int colors = g.ColorStack.Size, vars = g.StyleVarStack.Size;
		try { RenderAll(); }
		catch (...)
		{
			while (g.CurrentWindowStack.Size > depth)
			{
				while (g.CurrentTable && (g.CurrentTable->OuterWindow == g.CurrentWindow || g.CurrentTable->InnerWindow == g.CurrentWindow)) { ImGui::EndTable(); }
				ImGuiWindow* w = g.CurrentWindow;
				while (g.CurrentTabBar) { ImGui::EndTabBar(); }
				while (w->DC.TreeDepth > 0) { ImGui::TreePop(); }
				while (g.GroupStack.Size > w->DC.StackSizesOnBegin.SizeOfGroupStack) { ImGui::EndGroup(); }
				while (w->IDStack.Size > 1) { ImGui::PopID(); }
				while (g.ColorStack.Size > w->DC.StackSizesOnBegin.SizeOfColorStack) { ImGui::PopStyleColor(); }
				while (g.StyleVarStack.Size > w->DC.StackSizesOnBegin.SizeOfStyleVarStack) { ImGui::PopStyleVar(); }
				if (w->Flags & ImGuiWindowFlags_ChildWindow) { ImGui::EndChild(); } else { ImGui::End(); }
			}
			while (g.ColorStack.Size > colors) { ImGui::PopStyleColor(); }
			while (g.StyleVarStack.Size > vars) { ImGui::PopStyleVar(); }
			throw;
		}
		s_FrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		s_FrameAvg = s_FrameCount++ ? s_FrameAvg * 0.98 + s_FrameMs * 0.02 : s_FrameMs;
		s_FrameWorst = std::max(s_FrameWorst * 0.999, s_FrameMs);
	}

	namespace
	{
	void RenderAll()
	{
		if (!s_Gameplay) { s_Hovered = false; return; }
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
		SetDataVersion(snap.Version);
		if (snap.Fights.empty())
		{
			FirstRun(snap);
			ImGui::End();
			return;
		}
		State& s = S();
		if (ForcedMetric >= 0) { s.Metric = ForcedMetric; }
		if (ForcedOpen >= 0) { s.OpenFix = ForcedOpen; }
		int count = static_cast<int>(snap.Fights.size());
		int index = s.Selected < 0 || s.Selected >= count ? count - 1 : s.Selected;

		HeaderRow(snap, index);
		if (!snap.Status.empty()) { ImGui::TextColored(kMuted, "%s", snap.Status.c_str()); }

		// a new round while following the latest: open on its Summary (the v6 design)
		static int seen = 0;
		// (not while the logs on disk load at start: they come in one by one)
		if (count > seen && seen > 0 && !snap.Loading && s.Selected < 0 && s.SwitchTo < 0) { s.SwitchTo = T_Summary; }
		seen = count;
		int want = ForcedTab >= 0 ? ForcedTab : s.SwitchTo;
		s.SwitchTo = -1;
		auto tab = [want](const char* aName, int aIndex)
		{
			return ImGui::BeginTabItem(aName, nullptr, want == aIndex ? ImGuiTabItemFlags_SetSelected : 0);
		};
		if (ImGui::BeginTabBar("tabs"))
		{
			if (tab("Summary", T_Summary)) { SummaryTab(CachedCtx(snap.Fights, index, false, ForcedYou)); ImGui::EndTabItem(); }
			if (tab("You", T_You))
			{
				ScopeSwitch("you", s.YouTonight, "Your night: the fixes that keep coming back, round by round");
				if (s.YouTonight)
				{
					// the night doesn't depend on the round picked: a round you took no part in falls back to your latest one
					int night = index;
					if (snap.Fights[night]->Pov < 0) { for (int i = count - 1; i >= 0; i--) { if (snap.Fights[i]->Pov >= 0) { night = i; break; } } }
					TonightTab(CachedCtx(snap.Fights, night, true, ForcedYou));
				}
				else { ReviewTab(CachedCtx(snap.Fights, index, false, ForcedYou)); }
				ImGui::EndTabItem();
			}
			if (tab("Deaths", T_Deaths)) { DeathsTab(CachedCtx(snap.Fights, index, false, ForcedYou)); ImGui::EndTabItem(); }
			if (tab("Compare", T_Compare))
			{
				ScopeSwitch("compare", s.CompareTonight, "Every round loaded, added up");
				if (s.CompareLeft.empty() && s.CompareRight.empty()) { CompareTab(CachedCtx(snap.Fights, index, s.CompareTonight, ForcedYou)); }
				else { CompareTab(CachedCtx(snap.Fights, index, s.CompareTonight, s.CompareLeft, s.CompareRight)); }
				ImGui::EndTabItem();
			}
			if (tab("Round", T_Round)) { RoundTab(CachedCtx(snap.Fights, index, false, ForcedYou)); ImGui::EndTabItem(); }
			if (tab("Squad", T_Squad))
			{
				// one round only: its tables are the round's players (the Tonight switch did nothing; the user, 2026-09-30)
				SquadTab(CachedCtx(snap.Fights, index, false, ForcedYou));
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
		ImGui::Text("Log folder: %s", Session::PathText(snap.Folder).c_str());
		ImGui::Text("Rounds loaded: %d", static_cast<int>(snap.Fights.size()));
		ImGui::Checkbox("Show the Fight Review window", &ShowWindow);
		if (ImGui::Checkbox("Show the small summary window", &ShowMini)) { SaveSettings(); }
		ImGui::Text("Drawing the windows costs %.2f ms a frame (average %.2f, recent worst %.1f)", s_FrameMs, s_FrameAvg, s_FrameWorst);
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("At 60 fps a frame has 16.7 ms"); }
	}
}
