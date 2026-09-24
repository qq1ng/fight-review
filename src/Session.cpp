#include "Session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

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

		void Run(fs::path aFolder, int aLookbackHours)
		{
			std::set<fs::path> done;
			std::unordered_map<std::string, uintmax_t> lastSize; // a log counts as written when its size holds
			const auto since = Clock::now() - std::chrono::hours(aLookbackHours);
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
				for (auto& [written, path] : ready)
				{
					if (s_Stop) { break; }
					done.insert(path);
					SetStatus("Reading " + path.filename().string());
					try
					{
						auto fight = std::make_shared<Analysis::Fight>(Analysis::Analyse(path));
						Publish(std::move(fight));
						SetStatus("");
					}
					catch (const std::exception& e)
					{
						SetStatus(path.filename().string() + ": " + e.what());
					}
				}
				std::unique_lock lock(s_Mutex);
				s_Wake.wait_for(lock, std::chrono::seconds(2), [] { return s_Stop.load(); });
			}
		}
	}

	void Start(const fs::path& aFolder, int aLookbackHours)
	{
		Stop();
		{
			std::lock_guard lock(s_Mutex);
			s_Data = Snapshot{};
			s_Data.Folder = aFolder;
		}
		s_Stop = false;
		s_Thread = std::thread(Run, aFolder, aLookbackHours);
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
