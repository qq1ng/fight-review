#pragma once
// The boon evidence (which skills give which boons) kept between sessions in the addon's folder, with the logs it
// came from, so a restart doesn't count a log twice. A plain text file; unreadable or from another version: start
// empty.

#include <filesystem>
#include <set>
#include <string>

#include "Analysis.h"

namespace Analysis
{
	struct EvidenceStore
	{
		Evidence Pool;
		std::set<std::string> Logs; // stamps of the logs counted in Pool
	};

	EvidenceStore LoadEvidence(const std::filesystem::path& aPath);
	bool SaveEvidence(const std::filesystem::path& aPath, const EvidenceStore& aStore);
}
