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

	// At start the last play session loads: the newest log and every one before it back to a break of
	// aSessionGapHours or more between two logs. New logs load as they are written.
	// aEvidenceFile: which skills gave which boons, learned from every log read so far and kept between sessions
	// (empty: learn from each round alone).
	void Start(const std::filesystem::path& aFolder, int aSessionGapHours, const std::filesystem::path& aEvidenceFile = {});
	void Stop();
	Snapshot Get();

	// For the offscreen render harness: show these fights, no watching.
	void SetForTest(std::vector<FightPtr> aFights);
}
