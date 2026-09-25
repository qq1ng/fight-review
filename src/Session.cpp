#include "Session.h"

#include "EvidenceFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Session
{
	namespace
	{
		namespace fs = std::filesystem;
		using Clock = fs::file_time_type::clock;

		std::mutex              s_Mutex;
		std::condition_variable s_Wake;
		std::thread             s_Thread;
		std::atomic<bool>       s_Stop{false};
		Snapshot                s_Data; // guarded by s_Mutex

		void SetStatus(const std::string& aStatus)
		{
			std::lock_guard lock(s_Mutex);
			s_Data.Status = aStatus;
		}

		void Publish(FightPtr aFight)
		{
			std::lock_guard lock(s_Mutex);
			s_Data.Fights.push_back(std::move(aFight));
			std::sort(s_Data.Fights.begin(), s_Data.Fights.end(), [](const FightPtr& a, const FightPtr& b)
				{ return a->Stamp < b->Stamp; });
		}

		// The start of the last play session: from the newest log back, until two logs are aSessionGapHours or more
		// apart. So the evening loads whole however long it ran, and still loads the morning after.
		fs::file_time_type SessionStart(const fs::path& aFolder, int aSessionGapHours)
		{
			std::vector<fs::file_time_type> times;
			std::error_code ec;
			for (auto it = fs::recursive_directory_iterator(aFolder, fs::directory_options::skip_permission_denied, ec);
				!ec && it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				const fs::path& p = it->path();
				if (p.extension() != ".zevtc" && p.extension() != ".evtc") { continue; }
				auto written = fs::last_write_time(p, ec);
				if (ec) { ec.clear(); continue; }
				times.push_back(written);
			}
			if (times.empty()) { return Clock::now(); }
			std::sort(times.rbegin(), times.rend());
			size_t i = 0;
			while (i + 1 < times.size() && times[i] - times[i + 1] < std::chrono::hours(aSessionGapHours)) { i++; }
			return times[i];
		}

		// Replace a round already shown (re-read with more evidence)
		void Replace(FightPtr aFight)
		{
			std::lock_guard lock(s_Mutex);
			for (FightPtr& f : s_Data.Fights) { if (f->Stamp == aFight->Stamp) { f = std::move(aFight); return; } }
		}

		void Run(fs::path aFolder, int aSessionGapHours, fs::path aEvidenceFile)
		{
			// Boon evidence from every log read so far (this session and earlier ones)
			Analysis::EvidenceStore store;
			if (!aEvidenceFile.empty()) { store = Analysis::LoadEvidence(aEvidenceFile); }
			bool firstBatch = true;
			std::set<fs::path> done;
			std::unordered_map<std::string, uintmax_t> lastSize; // a log counts as written when its size holds
			const auto since = SessionStart(aFolder, aSessionGapHours);
			while (!s_Stop)
			{
				std::vector<std::pair<fs::file_time_type, fs::path>> ready;
				std::error_code ec;
				if (fs::exists(aFolder, ec))
				{
					for (auto it = fs::recursive_directory_iterator(aFolder, fs::directory_options::skip_permission_denied, ec);
						!ec && it != fs::recursive_directory_iterator(); it.increment(ec))
					{
						const fs::path& p = it->path();
						if (p.extension() != ".zevtc" && p.extension() != ".evtc") { continue; }
						if (done.count(p)) { continue; }
						auto written = fs::last_write_time(p, ec);
						if (ec || written < since) { ec.clear(); continue; }
						uintmax_t size = fs::file_size(p, ec);
						if (ec) { ec.clear(); continue; }
						auto& last = lastSize[p.string()];
						bool settled = last == size && Clock::now() - written > std::chrono::seconds(2);
						last = size;
						if (settled) { ready.push_back({written, p}); }
					}
				}
				else
				{
					SetStatus("Log folder not found: " + aFolder.string());
				}
				std::sort(ready.begin(), ready.end());
				int added = 0;
				std::vector<fs::path> read;
				for (auto& [written, path] : ready)
				{
					if (s_Stop) { break; }
					done.insert(path);
					SetStatus("Reading " + path.filename().string());
					try
					{
						const std::string stamp = path.stem().string();
						const bool counted = store.Logs.count(stamp) > 0;
						auto fight = std::make_shared<Analysis::Fight>(Analysis::Analyse(path, &store.Pool, counted));
						// ArcDPS sometimes saves a second or so after a fight as a log of its own: no enemy players, one
						// squad member at most. Not a round (2026-09-24: it showed as "the recorder isn't in the squad").
						if (fight->EnemyCount == 0 || fight->SquadCount < 2)
						{
							SetStatus("Skipped " + path.filename().string() + ": no fight in it");
							continue;
						}
						if (!counted)
						{
							Analysis::AddEvidence(store.Pool, fight->OwnEvidence);
							store.Logs.insert(stamp);
							added++;
						}
						fight->OwnEvidence = {}; // in the pool now; a round's copy is large
						read.push_back(path);
						Publish(std::move(fight));
						SetStatus("");
					}
					catch (const std::exception& e)
					{
						SetStatus(path.filename().string() + ": " + e.what());
					}
				}
				if (added && !aEvidenceFile.empty()) { Analysis::SaveEvidence(aEvidenceFile, store); }
				// The rounds loaded at start learned only from what came before each: read them again with the whole
				// evening's evidence (later rounds already have what came before them, which is most of it)
				if (firstBatch && added > 1 && !s_Stop)
				{
					for (size_t i = 0; i < read.size() && !s_Stop; i++)
					{
						SetStatus("Rereading " + std::to_string(i + 1) + " of " + std::to_string(read.size()) + " with tonight's boon evidence");
						try
						{
							auto fight = std::make_shared<Analysis::Fight>(Analysis::Analyse(read[i], &store.Pool, true));
							fight->OwnEvidence = {};
							Replace(std::move(fight));
						}
						catch (const std::exception&) {} // read fine the first time; keep that version
					}
					SetStatus("");
				}
				if (!ready.empty()) { firstBatch = false; }
				std::unique_lock lock(s_Mutex);
				s_Wake.wait_for(lock, std::chrono::seconds(2), [] { return s_Stop.load(); });
			}
		}
	}

	void Start(const fs::path& aFolder, int aSessionGapHours, const fs::path& aEvidenceFile)
	{
		Stop();
		{
			std::lock_guard lock(s_Mutex);
			s_Data = Snapshot{};
			s_Data.Folder = aFolder;
		}
		s_Stop = false;
		s_Thread = std::thread(Run, aFolder, aSessionGapHours, aEvidenceFile);
	}

	void Stop()
	{
		s_Stop = true;
		s_Wake.notify_all();
		if (s_Thread.joinable()) { s_Thread.join(); }
	}

	Snapshot Get()
	{
		std::lock_guard lock(s_Mutex);
		return s_Data;
	}

	void SetForTest(std::vector<FightPtr> aFights)
	{
		std::lock_guard lock(s_Mutex);
		s_Data.Fights = std::move(aFights);
		s_Data.Folder = "(test)";
	}
}
