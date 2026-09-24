#include "Icons.h"

#include <string>
#include <unordered_map>

#include <Windows.h>

#include "nexus/Nexus.h"

namespace Icons
{
	namespace
	{
		struct IconBlob
		{
			uint32_t             Profession;
			uint32_t             Specialization; // 0 for the core profession icon
			const unsigned char* Data;
			size_t               Size;
		};

#include "IconData.inc"

		AddonAPI_t*                               s_Api = nullptr;
		std::unordered_map<uint32_t, void*>       s_Loaded; // key -> shader resource view

		uint32_t Key(uint32_t aProfession, uint32_t aSpecialization) { return aSpecialization ? 1000 + aSpecialization : aProfession; }

		// Nexus creates textures from memory on the render thread; the first calls may return null.
		void* Load(const IconBlob& aBlob)
		{
			uint32_t key = Key(aBlob.Profession, aBlob.Specialization);
			auto it = s_Loaded.find(key);
			if (it != s_Loaded.end()) { return it->second; }

			std::string identifier = "FIGHTREVIEW_ICON_" + std::to_string(key);
			Texture_t* texture = s_Api->Textures_GetOrCreateFromMemory(identifier.c_str(), const_cast<unsigned char*>(aBlob.Data), aBlob.Size);
			if (texture == nullptr || texture->Resource == nullptr) { return nullptr; }
			s_Loaded[key] = texture->Resource;
			return texture->Resource;
		}
	}

	void Init(AddonAPI_t* aApi)
	{
		s_Api = aApi;
	}

	void Shutdown()
	{
		s_Loaded.clear();
		s_Api = nullptr;
	}

	void* Get(uint32_t aProfession, uint32_t aElite)
	{
		if (s_Api == nullptr || aProfession == 0) { return nullptr; }
		const IconBlob* core = nullptr;
		for (const IconBlob& blob : kIconBlobs)
		{
			if (blob.Specialization != 0 && blob.Specialization == aElite && blob.Profession == aProfession)
			{
				if (void* icon = Load(blob)) { return icon; }
			}
			if (blob.Specialization == 0 && blob.Profession == aProfession) { core = &blob; }
		}
		return core ? Load(*core) : nullptr;
	}
}
