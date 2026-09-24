#include "SkillIcons.h"

#include <atomic>
#include <cctype>
#include <iterator>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include <winhttp.h>

#include "nexus/Nexus.h"

#pragma comment(lib, "winhttp.lib")

namespace SkillIcons
{
	namespace
	{
		AddonAPI_t*             s_Api = nullptr;
		std::filesystem::path   s_Cache;
		std::mutex              s_Mutex;
		std::condition_variable s_Wake;
		std::thread             s_Thread;
		std::atomic<bool>       s_Stop{false};
		std::unordered_map<int32_t, std::string> s_Urls;  // skill -> icon URL ("" = the API has none)
		std::set<int32_t>       s_Wanted;                 // asked for, not yet fetched
		std::unordered_map<int32_t, void*> s_Textures;   // render thread only

#include "SkillIconData.inc"

		// Elite Insights' icon for a skill: by id, else by name ("Relic of Karakosa" -> "relicofkarakosa")
		const char* KnownUrl(int32_t aSkill, const std::string& aName)
		{
			static const std::unordered_map<int32_t, const char*> byId(std::begin(kIconById), std::end(kIconById));
			static const std::unordered_map<std::string, const char*> byName(std::begin(kIconByName), std::end(kIconByName));
			if (auto it = byId.find(aSkill); it != byId.end()) { return it->second; }
			std::string key;
			for (char c : aName) { if (std::isalnum(static_cast<unsigned char>(c))) { key += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } }
			if (auto it = byName.find(key); it != byName.end()) { return it->second; }
			return nullptr;
		}

		std::string Get(const std::wstring& aHost, const std::wstring& aPath)
		{
			std::string body;
			HINTERNET session = WinHttpOpen(L"FightReview/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS, 0);
			if (!session) { return body; }
			WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
			HINTERNET connect = WinHttpConnect(session, aHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
			HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", aPath.c_str(), nullptr, WINHTTP_NO_REFERER,
				WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
			if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
				WinHttpReceiveResponse(request, nullptr))
			{
				DWORD available = 0;
				while (WinHttpQueryDataAvailable(request, &available) && available > 0)
				{
					std::string chunk(available, '\0');
					DWORD read = 0;
					if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) { break; }
					body.append(chunk.data(), read);
				}
			}
			if (request) { WinHttpCloseHandle(request); }
			if (connect) { WinHttpCloseHandle(connect); }
			WinHttpCloseHandle(session);
			return body;
		}

		// Pulls each skill's top-level "id" and "icon" out of /v2/skills?ids=... (an array of objects; nested
		// objects such as facts are skipped, and key order doesn't matter).
		std::unordered_map<int32_t, std::string> Parse(const std::string& aJson)
		{
			std::unordered_map<int32_t, std::string> out;
			int depth = 0;
			int32_t id = -1;
			std::string icon;
			for (size_t i = 0; i < aJson.size(); i++)
			{
				char c = aJson[i];
				if (c == '"')
				{
					size_t end = i + 1;
					while (end < aJson.size() && aJson[end] != '"') { end += aJson[end] == '\\' ? 2 : 1; }
					std::string text = aJson.substr(i + 1, end - i - 1);
					i = end;
					if (depth != 2) { continue; } // depth 1: the array, 2: a skill object
					size_t colon = aJson.find_first_not_of(" \t\r\n", i + 1);
					if (colon == std::string::npos || aJson[colon] != ':') { continue; }
					size_t value = aJson.find_first_not_of(" \t\r\n", colon + 1);
					if (value == std::string::npos) { break; }
					if (text == "id") { id = std::atoi(aJson.c_str() + value); }
					else if (text == "icon" && aJson[value] == '"')
					{
						size_t close = aJson.find('"', value + 1);
						if (close != std::string::npos) { icon = aJson.substr(value + 1, close - value - 1); }
					}
				}
				else if (c == '{' || c == '[') { depth++; }
				else if (c == '}' || c == ']')
				{
					if (depth == 2 && c == '}' && id >= 0) { out[id] = icon; }
					if (depth == 2) { id = -1; icon.clear(); }
					depth--;
				}
			}
			return out;
		}

		void SaveCache()
		{
			std::ofstream out(s_Cache, std::ios::trunc);
			for (auto& [id, url] : s_Urls) { out << id << ' ' << url << '\n'; }
		}

		void Run()
		{
			while (!s_Stop)
			{
				std::vector<int32_t> batch;
				{
					std::unique_lock lock(s_Mutex);
					s_Wake.wait_for(lock, std::chrono::seconds(3), [] { return s_Stop.load() || !s_Wanted.empty(); });
					for (auto it = s_Wanted.begin(); it != s_Wanted.end() && batch.size() < 150;) { batch.push_back(*it); it = s_Wanted.erase(it); }
				}
				if (batch.empty() || s_Stop) { continue; }
				std::wstring path = L"/v2/skills?ids=";
				for (size_t i = 0; i < batch.size(); i++) { path += (i ? L"," : L"") + std::to_wstring(batch[i]); }
				std::string json = Get(L"api.guildwars2.com", path);
				if (json.empty()) { continue; } // offline: ask again next time the skill is drawn
				auto found = Parse(json);
				std::lock_guard lock(s_Mutex);
				for (int32_t id : batch) { s_Urls[id] = found.count(id) ? found[id] : std::string(); }
				SaveCache();
			}
		}
	}

	void Init(AddonAPI_t* aApi, const std::filesystem::path& aCacheFile)
	{
		s_Api = aApi;
		s_Cache = aCacheFile;
		std::ifstream in(aCacheFile);
		int32_t id;
		std::string url;
		while (in >> id)
		{
			std::getline(in, url);
			if (!url.empty() && url[0] == ' ') { url.erase(0, 1); }
			s_Urls[id] = url;
		}
		s_Stop = false;
		s_Thread = std::thread(Run);
	}

	void Shutdown()
	{
		s_Stop = true;
		s_Wake.notify_all();
		if (s_Thread.joinable()) { s_Thread.join(); }
		s_Textures.clear();
		s_Api = nullptr;
	}

	void* Get(int32_t aSkill, const std::string& aName)
	{
		if (!s_Api || aSkill == 0) { return nullptr; }
		if (auto it = s_Textures.find(aSkill); it != s_Textures.end() && it->second) { return it->second; }
		std::string url;
		if (const char* known = KnownUrl(aSkill, aName)) { url = known; }
		else if (aSkill > 0)
		{
			std::lock_guard lock(s_Mutex);
			auto it = s_Urls.find(aSkill);
			if (it == s_Urls.end()) { s_Wanted.insert(aSkill); s_Wake.notify_all(); return nullptr; }
			url = it->second;
		}
		// "https://host/path" -> remote "https://host", endpoint "/path" (render.guildwars2.com or the wiki)
		size_t scheme = url.find("://");
		size_t slash = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
		if (slash == std::string::npos) { return nullptr; }
		std::string id = "FIGHTREVIEW_SKILL_" + std::to_string(aSkill);
		Texture_t* texture = s_Api->Textures_GetOrCreateFromURL(id.c_str(), url.substr(0, slash).c_str(), url.substr(slash).c_str());
		void* resource = texture ? texture->Resource : nullptr;
		if (resource) { s_Textures[aSkill] = resource; }
		return resource;
	}
}
