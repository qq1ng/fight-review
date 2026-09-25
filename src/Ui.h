#pragma once

#include <filesystem>
#include <string>

namespace Ui
{
	constexpr const char* kWindowName = "Fight Review";
	extern bool ShowWindow;
	extern int ForcedTab;    // render harness: select this tab (Review, Compare, Squad, Fight), -1 = the player's choice
	extern int ForcedMetric; // render harness: Compare's metric, -1 = the player's choice
	extern int ForcedOpen;   // render harness: open this Review fix line, -1 = the player's choice
	extern std::string ForcedYou; // render harness: review the round as this account instead of the log's recorder
	constexpr int kForceReviveOrder = 99; // render harness: ForcedOpen value that opens the revive order and every down
	extern bool ShowMini;    // the small summary window of the latest round

	void LoadSettings(const std::filesystem::path& aFile); // settings.txt in the addon folder; saved on change
	void Render();  // the window, if shown
	void Options(); // Nexus options page

	// A mouse wheel turn from the window procedure (WHEEL_DELTA units). True when it was over our window: it will
	// scroll it, and the game shouldn't zoom the camera.
	bool OnMouseWheel(int aDelta);
}
