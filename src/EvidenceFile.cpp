#include "EvidenceFile.h"

#include <fstream>
#include <sstream>

namespace Analysis
{
	namespace
	{
		// Bump when the evidence means something else (new fields, another rule for what counts as a use)
		const char* kHeader = "fight-review boon evidence 1";

		void WriteCounts(std::ostream& aOut, const BoonEvidence& ev, bool aLags)
		{
			aOut << ev.Uses;
			for (int b = 0; b < kBoons; b++) { aOut << ' ' << ev.Followed[b]; }
			for (int b = 0; b < kBoons; b++) { aOut << ' ' << ev.Expected[b]; }
			if (!aLags) { return; }
			for (int b = 0; b < kBoons; b++) { for (int k = 0; k < kLagSeconds; k++) { aOut << ' ' << ev.After[b][k]; } }
			for (int b = 0; b < kBoons; b++) { for (int k = 0; k < kLagSeconds; k++) { aOut << ' ' << ev.AfterExpected[b][k]; } }
		}

		bool ReadCounts(std::istream& aIn, BoonEvidence& ev, bool aLags)
		{
			aIn >> ev.Uses;
			for (int b = 0; b < kBoons; b++) { aIn >> ev.Followed[b]; }
			for (int b = 0; b < kBoons; b++) { aIn >> ev.Expected[b]; }
			if (aLags)
			{
				for (int b = 0; b < kBoons; b++) { for (int k = 0; k < kLagSeconds; k++) { aIn >> ev.After[b][k]; } }
				for (int b = 0; b < kBoons; b++) { for (int k = 0; k < kLagSeconds; k++) { aIn >> ev.AfterExpected[b][k]; } }
			}
			return static_cast<bool>(aIn) && ev.Uses > 0;
		}
	}

	// Lines: "log <stamp>", "prof <profession> <skill> <counts with lags>", "player <skill> <counts> <account>"
	// (the account last: it can hold spaces)
	EvidenceStore LoadEvidence(const std::filesystem::path& aPath)
	{
		EvidenceStore store;
		std::ifstream in(aPath);
		std::string line;
		if (!in || !std::getline(in, line) || line != kHeader) { return store; }
		while (std::getline(in, line))
		{
			std::istringstream ls(line);
			std::string kind;
			ls >> kind;
			if (kind == "log")
			{
				std::string stamp;
				if (ls >> stamp) { store.Logs.insert(stamp); }
			}
			else if (kind == "prof")
			{
				uint64_t prof = 0;
				int32_t skill = 0;
				BoonEvidence ev;
				if (ls >> prof >> skill && ReadCounts(ls, ev, true)) { store.Pool.ByProfession[prof << 32 | static_cast<uint32_t>(skill)] = ev; }
			}
			else if (kind == "player")
			{
				int32_t skill = 0;
				BoonEvidence ev;
				std::string account;
				if (ls >> skill && ReadCounts(ls, ev, false) && (ls >> std::ws, std::getline(ls, account)) && !account.empty())
				{
					store.Pool.ByPlayer[{account, skill}] = ev;
				}
			}
		}
		return store;
	}

	bool SaveEvidence(const std::filesystem::path& aPath, const EvidenceStore& aStore)
	{
		std::filesystem::path temp = aPath;
		temp += ".tmp";
		{
			std::ofstream out(temp, std::ios::trunc);
			if (!out) { return false; }
			out << kHeader << '\n';
			for (const std::string& stamp : aStore.Logs) { out << "log " << stamp << '\n'; }
			for (auto& [key, ev] : aStore.Pool.ByProfession)
			{
				out << "prof " << (key >> 32) << ' ' << static_cast<int32_t>(key & 0xFFFFFFFF) << ' ';
				WriteCounts(out, ev, true);
				out << '\n';
			}
			for (auto& [key, ev] : aStore.Pool.ByPlayer)
			{
				out << "player " << key.second << ' ';
				WriteCounts(out, ev, false);
				out << ' ' << key.first << '\n';
			}
			if (!out) { return false; }
		}
		std::error_code ec;
		std::filesystem::rename(temp, aPath, ec); // replace the old file only once the new one is complete
		return !ec;
	}
}
