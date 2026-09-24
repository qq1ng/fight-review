#pragma once
// Watches the ArcDPS log folder on a worker thread and analyses every new log. The UI reads a snapshot.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Analysis.h"

namespace Session
{
	using FightPtr = std::shared_ptr<const Analysis::Fight>;

	struct Snapshot
	{
		std::vector<FightPtr> Fights; // oldest first
		std::string           Status; // one line: what the worker is doing or last went wrong
		std::filesystem::path Folder;
	};

	// Logs written in the last aLookbackHours are loaded at start, so a reload mid-raid keeps the evening.
	void Start(const std::filesystem::path& aFolder, int aLookbackHours);
	void Stop();
	Snapshot Get();

	// For the offscreen render harness: show these fights, no watching.
	void SetForTest(std::vector<FightPtr> aFights);
}
