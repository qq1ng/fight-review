#pragma once

#include <cstdint>

struct AddonAPI_t;

// Profession and elite specialization icons (embedded PNGs, loaded as Nexus textures). Render thread only.
namespace Icons
{
	void Init(AddonAPI_t* aApi);
	void Shutdown();

	// ID3D11ShaderResourceView* for the player's elite specialization, else the core profession icon,
	// or null while loading / unknown.
	void* Get(uint32_t aProfession, uint32_t aElite);
}
