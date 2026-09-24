// Nexus addon entry: load/unload, the window, the options page. Skeleton taken from Rezz Order.
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <Windows.h>
#include <ShlObj.h>

#include "imgui/imgui.h"
#include "nexus/Nexus.h"

#include "Icons.h"
#include "Session.h"
#include "SkillIcons.h"
#include "Ui.h"

#define ADDON_NAME "Fight Review"

namespace
{
	constexpr const char* KB_TOGGLE = "KB_FIGHTREVIEW_TOGGLE";
	constexpr const char* QA_MENU_ITEM = "QA_FIGHTREVIEW_MENU";
	constexpr int kLookbackHours = 8;

	AddonDefinition_t s_AddonDef{};
	AddonAPI_t*       s_Api = nullptr;

	// Callbacks come from Nexus and the game's window procedure: C boundaries, so nothing may throw through
	// them. Counting calls in flight lets Unload wait for them.
	std::atomic<bool> s_Alive{false};
	std::atomic<int>  s_InFlight{0};
	std::atomic<int>  s_Failures{0};

	template <typename F>
	void Guarded(const char* aWhat, F&& aFunction)
	{
		s_InFlight.fetch_add(1);
		if (s_Alive.load())
		{
			try { aFunction(); }
			catch (const std::exception& e)
			{
				if (s_Failures.fetch_add(1) < 20 && s_Api)
				{
					std::string m = std::string("Caught an exception in ") + aWhat + ": " + e.what();
					s_Api->Log(LOGL_WARNING, ADDON_NAME, m.c_str());
				}
			}
			catch (...) { s_Failures.fetch_add(1); }
		}
		s_InFlight.fetch_sub(1);
	}

	void OnRender() { Guarded("the window", [] { Ui::Render(); }); }
	void OnOptions() { Guarded("the options page", [] { Ui::Options(); }); }
	void OnQuickAccessMenu()
	{
		Guarded("the quick access menu", [] { if (ImGui::Button("Fight Review")) { Ui::ShowWindow = !Ui::ShowWindow; } });
	}
	void OnInputBind(const char* aIdentifier, bool aIsRelease)
	{
		Guarded("the key bind", [aIdentifier, aIsRelease]
		{
			if (!aIsRelease && std::strcmp(aIdentifier, KB_TOGGLE) == 0) { Ui::ShowWindow = !Ui::ShowWindow; }
		});
	}

	UINT OnWndProc(HWND, UINT aMsg, WPARAM aWParam, LPARAM)
	{
		bool consumed = false;
		Guarded("the window procedure", [&]
		{
			if (aMsg == WM_MOUSEWHEEL) { consumed = Ui::OnMouseWheel(GET_WHEEL_DELTA_WPARAM(aWParam)); }
		});
		return consumed ? 0 : aMsg; // 0: handled, the game doesn't see it
	}

	// ArcDPS saves to boss_encounter_path from arcdps.ini, or to Documents\Guild Wars 2\addons\arcdps\arcdps.cbtlogs.
	std::filesystem::path LogFolder()
	{
		std::filesystem::path ini = std::filesystem::path(s_Api->Paths_GetAddonDirectory("arcdps")) / "arcdps.ini";
		std::ifstream in(ini);
		std::string line;
		while (std::getline(in, line))
		{
			const std::string key = "boss_encounter_path=";
			if (line.rfind(key, 0) == 0 && line.size() > key.size())
			{
				std::string value = line.substr(key.size());
				while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) { value.pop_back(); }
				if (!value.empty()) { return std::filesystem::path(value) / "arcdps.cbtlogs"; }
			}
		}
		PWSTR docs = nullptr;
		std::filesystem::path out;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)))
		{
			out = std::filesystem::path(docs) / "Guild Wars 2" / "addons" / "arcdps" / "arcdps.cbtlogs";
		}
		CoTaskMemFree(docs);
		return out;
	}

	void AddonLoad(AddonAPI_t* aApi)
	{
		s_Api = aApi;
		ImGui::SetCurrentContext(static_cast<ImGuiContext*>(s_Api->ImguiContext));
		ImGui::SetAllocatorFunctions(
			static_cast<void* (*)(size_t, void*)>(s_Api->ImguiMalloc),
			static_cast<void (*)(void*, void*)>(s_Api->ImguiFree));

		std::filesystem::path addonDir = s_Api->Paths_GetAddonDirectory("FightReview");
		std::error_code ec;
		std::filesystem::create_directories(addonDir, ec);
		Icons::Init(s_Api);
		SkillIcons::Init(s_Api, addonDir / "skill_icons.txt");

		std::filesystem::path folder = LogFolder();
		Session::Start(folder, kLookbackHours);
		std::string msg = "Watching " + folder.string();
		s_Api->Log(LOGL_INFO, ADDON_NAME, msg.c_str());

		s_Alive = true;
		s_Api->WndProc_Register(OnWndProc);
		s_Api->InputBinds_RegisterWithString(KB_TOGGLE, OnInputBind, "CTRL+SHIFT+F");
		s_Api->GUI_Register(RT_Render, OnRender);
		s_Api->GUI_Register(RT_OptionsRender, OnOptions);
		s_Api->GUI_RegisterCloseOnEscape(Ui::kWindowName, &Ui::ShowWindow);
		s_Api->QuickAccess_AddContextMenu(QA_MENU_ITEM, "QA_MENU", OnQuickAccessMenu);
		s_Api->Log(LOGL_INFO, ADDON_NAME, "Loaded.");
	}

	void AddonUnload()
	{
		s_Alive = false;
		for (int spins = 0; s_InFlight.load() > 0 && spins < 1000; spins++) { ::Sleep(1); }
		s_Api->QuickAccess_RemoveContextMenu(QA_MENU_ITEM);
		s_Api->GUI_DeregisterCloseOnEscape(Ui::kWindowName);
		s_Api->GUI_Deregister(OnOptions);
		s_Api->GUI_Deregister(OnRender);
		s_Api->InputBinds_Deregister(KB_TOGGLE);
		s_Api->WndProc_Deregister(OnWndProc);
		Session::Stop();
		SkillIcons::Shutdown();
		Icons::Shutdown();
		s_Api = nullptr;
	}
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
	s_AddonDef.Signature = static_cast<uint32_t>(-48213911); // unique negative id for addons not hosted on Raidcore
	s_AddonDef.APIVersion = NEXUS_API_VERSION;
	s_AddonDef.Name = ADDON_NAME;
	s_AddonDef.Version.Major = FR_VERSION_MAJOR;
	s_AddonDef.Version.Minor = FR_VERSION_MINOR;
	s_AddonDef.Version.Build = FR_VERSION_PATCH;
	s_AddonDef.Version.Revision = 0;
	s_AddonDef.Author = "qq1ng";
	s_AddonDef.Description = "Reviews each fight from its ArcDPS log: you against your spec, the squad, skills and boons.";
	s_AddonDef.Load = AddonLoad;
	s_AddonDef.Unload = AddonUnload;
	s_AddonDef.Flags = AF_None;
	s_AddonDef.Provider = UP_None;
	return &s_AddonDef;
}
