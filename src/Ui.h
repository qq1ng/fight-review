#pragma once

namespace Ui
{
	constexpr const char* kWindowName = "Fight Review";
	extern bool ShowWindow;
	extern int ForcedTab;    // render harness: select this tab (Review, Compare, Squad, Fight), -1 = the player's choice
	extern int ForcedMetric; // render harness: Compare's metric, -1 = the player's choice
	extern int ForcedOpen;   // render harness: open this Review fix line, -1 = the player's choice

	void Render();  // the window, if shown
	void Options(); // Nexus options page

	// A mouse wheel turn from the window procedure (WHEEL_DELTA units). True when it was over our window: it will
	// scroll it, and the game shouldn't zoom the camera.
	bool OnMouseWheel(int aDelta);
}
