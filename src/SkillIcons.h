#pragma once
// Skill icons. The icon URL comes from Elite Insights' image tables (src/SkillIconData.inc: by skill id, then by
// the skill's name, which covers relics, boons, traits and combos), else from the public GW2 API (/v2/skills,
// fetched on a worker thread with WinHTTP and cached in the addon folder). Nexus loads the image itself.

#include <cstdint>
#include <filesystem>
#include <string>

struct AddonAPI_t;

namespace SkillIcons
{
	void Init(AddonAPI_t* aApi, const std::filesystem::path& aCacheFile);
	void Shutdown();

	// ID3D11ShaderResourceView* or null (unknown skill, still loading, no icon). Render thread.
	void* Get(int32_t aSkill, const std::string& aName);
}
