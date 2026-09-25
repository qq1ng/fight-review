// Renders the addon's real window offscreen from real logs and saves a PNG per tab, with ImGui's asserts on
// (they are off in the release DLL, where a mistake would take the game down instead).
//
//   build/release/fr_uishot.exe --out <dir> <log.zevtc> [more logs ...]
//   build/release/fr_uishot.exe --time <log.zevtc> [more logs ...]   (CPU ms per frame for each tab, no PNGs)
//   --as <spec>: review the latest round as a player on that spec; --jobs: each player's job list;
//   --round <part of a log name>: that round instead of the latest
//
// Exit code 1 if a render failed or an ImGui assert fired.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include "Analysis.h"
#include "Session.h"
#include "Ui.h"
#include "UiCommon.h"

void UishotAssert(const char* aExpr, const char* aFile, int aLine)
{
	std::printf("IMGUI ASSERT: %s (%s:%d)\n", aExpr, aFile, aLine);
	std::fflush(stdout);
	std::exit(3);
}

namespace
{
	constexpr int kWidth = 1280, kHeight = 900;
	ID3D11Device* s_Device = nullptr;
	ID3D11DeviceContext* s_Context = nullptr;
	ID3D11Texture2D* s_Target = nullptr;
	ID3D11RenderTargetView* s_Rtv = nullptr;
	ID3D11Texture2D* s_Staging = nullptr;

	bool CreateDevice()
	{
		const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
		D3D_FEATURE_LEVEL level{};
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 2, D3D11_SDK_VERSION,
			&s_Device, &level, &s_Context);
		if (FAILED(hr))
		{
			hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, wanted, 2, D3D11_SDK_VERSION, &s_Device,
				&level, &s_Context);
		}
		if (FAILED(hr)) { return false; }
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kWidth;
		desc.Height = kHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(s_Device->CreateTexture2D(&desc, nullptr, &s_Target))) { return false; }
		if (FAILED(s_Device->CreateRenderTargetView(s_Target, nullptr, &s_Rtv))) { return false; }
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		return SUCCEEDED(s_Device->CreateTexture2D(&desc, nullptr, &s_Staging));
	}

	bool SavePng(const std::wstring& aPath)
	{
		s_Context->CopyResource(s_Staging, s_Target);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(s_Context->Map(s_Staging, 0, D3D11_MAP_READ, 0, &mapped))) { return false; }
		std::vector<unsigned char> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
		for (int y = 0; y < kHeight; y++)
		{
			std::memcpy(pixels.data() + static_cast<size_t>(y) * kWidth * 4,
				static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, kWidth * 4);
		}
		for (size_t i = 3; i < pixels.size(); i += 4) { pixels[i] = 255; }
		s_Context->Unmap(s_Staging, 0);

		IWICImagingFactory* factory = nullptr;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { return false; }
		bool ok = false;
		IWICBitmap* bitmap = nullptr;
		IWICStream* stream = nullptr;
		IWICBitmapEncoder* encoder = nullptr;
		IWICBitmapFrameEncode* frame = nullptr;
		IWICFormatConverter* converter = nullptr;
		if (SUCCEEDED(factory->CreateBitmapFromMemory(kWidth, kHeight, GUID_WICPixelFormat32bppRGBA, kWidth * 4,
				static_cast<UINT>(pixels.size()), pixels.data(), &bitmap)) &&
			SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
			SUCCEEDED(converter->Initialize(bitmap, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
				WICBitmapPaletteTypeCustom)) &&
			SUCCEEDED(factory->CreateStream(&stream)) &&
			SUCCEEDED(stream->InitializeFromFilename(aPath.c_str(), GENERIC_WRITE)) &&
			SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
			SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
			SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
			SUCCEEDED(frame->SetSize(kWidth, kHeight)))
		{
			WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
			ok = SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(converter, nullptr)) &&
				SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
		}
		if (converter) { converter->Release(); }
		if (frame) { frame->Release(); }
		if (encoder) { encoder->Release(); }
		if (stream) { stream->Release(); }
		if (bitmap) { bitmap->Release(); }
		factory->Release();
		return ok;
	}

	double s_RenderMs = 0; // CPU time of the last Ui::Render()

	void Frame()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(kWidth, kHeight);
		io.DeltaTime = 1.0f / 60.0f;
		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(980, 860), ImGuiCond_Always);
		auto t0 = std::chrono::steady_clock::now();
		Ui::Render();
		s_RenderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		ImGui::Render();
		const float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
		s_Context->OMSetRenderTargets(1, &s_Rtv, nullptr);
		s_Context->ClearRenderTargetView(s_Rtv, clear);
		D3D11_VIEWPORT vp{0, 0, kWidth, kHeight, 0, 1};
		s_Context->RSSetViewports(1, &vp);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}
}

int main(int argc, char** argv)
{
	// --session <log folder> <evidence file>: run the real session worker (load, learn, save, reread) and report
	if (argc == 4 && std::strcmp(argv[1], "--session") == 0)
	{
		auto t0 = std::chrono::steady_clock::now();
		Session::Start(argv[2], 3, argv[3]);
		size_t last = 0;
		int quiet = 0;
		while (quiet < 6)
		{
			Sleep(1000);
			Session::Snapshot snap = Session::Get();
			quiet = snap.Fights.size() == last && snap.Status.empty() ? quiet + 1 : 0;
			last = snap.Fights.size();
			std::printf("%5.1f s  %zu rounds  %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), last, snap.Status.c_str());
		}
		Session::Stop();
		std::error_code ec;
		std::printf("evidence file: %llu bytes\n", static_cast<unsigned long long>(std::filesystem::file_size(argv[3], ec)));
		return 0;
	}
	std::string out = ".";
	bool timing = false, jobs = false;
	std::string as; // --as <spec>: review the latest round as a player on that spec
	std::string round; // --round <part of a log name>: review that round
	std::string down;  // --down <player name>: the Deaths tab opens on their last down in that round
	float scale = 1.0f; // --scale <x>: a wider font, like the game's
	std::vector<Session::FightPtr> fights;
	for (int i = 1; i < argc; i++)
	{
		if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) { out = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--time") == 0) { timing = true; continue; }
		if (std::strcmp(argv[i], "--jobs") == 0) { jobs = true; continue; }
		if (std::strcmp(argv[i], "--as") == 0 && i + 1 < argc) { as = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--round") == 0 && i + 1 < argc) { round = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--down") == 0 && i + 1 < argc) { down = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) { scale = static_cast<float>(std::atof(argv[++i])); continue; }
		try { fights.push_back(std::make_shared<Analysis::Fight>(Analysis::Analyse(argv[i]))); }
		catch (const std::exception& e) { std::printf("%s: %s\n", argv[i], e.what()); return 1; }
	}
	if (jobs)
	{
		// Each player's job list over these rounds, counted per spec and build (no names)
		std::map<std::string, int> seen;
		std::set<std::string> done;
		for (const auto& f : fights)
		{
			for (const auto& p : f->Players)
			{
				if (!done.insert(p.Account + "|" + p.Spec).second) { continue; }
				std::string build;
				std::vector<std::string> list = Ui::JobsFor(fights, p.Account, p.Spec, &build);
				std::string line = p.Spec + (build.empty() ? "" : " " + build) + ":";
				for (const std::string& j : list) { line += " " + j + ";"; }
				seen[line]++;
				if (p.Spec == "Troubadour")
				{
					auto [timed, all] = Ui::CastsOnEnemySpikes(fights, p.Account, p.Spec, Ui::kTaleOfTheAugustQueen);
					std::printf("Troubadour: %d of %d Tale of the August Queen casts timed with enemy spikes\n", timed, all);
				}
			}
		}
		for (auto& [line, n] : seen) { std::printf("%3d  %s\n", n, line.c_str()); }
		return 0;
	}
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (!CreateDevice()) { std::printf("no D3D11 device\n"); return 1; }
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().FontGlobalScale = scale;
	ImGui_ImplDX11_Init(s_Device, s_Context);

	Session::SetForTest(fights);
	Ui::ShowWindow = true;
	if (!as.empty())
	{
		for (const auto& p : fights.back()->Players) { if (p.Spec == as && !p.Pov) { Ui::ForcedYou = p.Account; break; } }
		std::printf("reviewing as a %s%s\n", as.c_str(), Ui::ForcedYou.empty() ? " (none in the latest round)" : "");
	}
	// --round <hhmm or stamp part>: review that round instead of the latest
	int picked = -1;
	for (int i = 0; i < static_cast<int>(fights.size()); i++) { if (!round.empty() && fights[i]->Stamp.find(round) != std::string::npos) { picked = i; } }
	if (!round.empty()) { std::printf("round %s: %s\n", round.c_str(), picked >= 0 ? fights[picked]->Stamp.c_str() : "not found, using the latest"); }
	Ui::S().Selected = picked >= 0 && picked < static_cast<int>(fights.size()) - 1 ? picked : -1;
	const Analysis::Fight& shown = *fights[picked >= 0 ? picked : fights.size() - 1];
	// --down Name or Name#n: their last down, or their n-th (from 0)
	int nth = -1;
	if (size_t hash = down.find('#'); hash != std::string::npos) { nth = std::atoi(down.c_str() + hash + 1); down.resize(hash); }
	for (const auto& p : shown.Players)
	{
		if (down.empty() || p.Name.rfind(down, 0) != 0) { continue; }
		int n = 0;
		for (const auto& sp : p.DownSpans)
		{
			if (sp.Dead) { continue; }
			if (nth < 0 || n == nth) { Ui::S().DeathKey = shown.Stamp + "/" + p.Account + "/" + std::to_string(sp.From); }
			n++;
		}
	}
	if (timing)
	{
		// The window is drawn every frame in game, so its CPU time per frame is what the player pays
		struct Case { const char* Name; int Tab; bool AllRounds; };
		const Case cases[] = {{"you, this round", Ui::T_You, false}, {"you, tonight", Ui::T_You, true}, {"deaths", Ui::T_Deaths, false},
			{"compare, this round", Ui::T_Compare, false}, {"compare, tonight", Ui::T_Compare, true}, {"round", Ui::T_Round, false},
			{"squad, this round", Ui::T_Squad, false}, {"squad, tonight", Ui::T_Squad, true}, {"tonight", Ui::T_Tonight, false}};
		for (const Case& k : cases)
		{
			Ui::ForcedTab = k.Tab;
			Ui::S().AllRounds = k.AllRounds;
			// the first frames build the tab's caches (a hitch when switching tab or when a round arrives)
			double first = 0;
			for (int n = 0; n < 3; n++) { Frame(); first = std::max(first, s_RenderMs); }
			double sum = 0, worst = 0;
			const int kFrames = 30;
			for (int n = 0; n < kFrames; n++) { Frame(); sum += s_RenderMs; worst = std::max(worst, s_RenderMs); }
			std::printf("%-22s %6.2f ms per frame (worst %.2f), first frames up to %.1f ms\n", k.Name, sum / kFrames, worst, first);
		}
		// The round with the most downs: Round and Deaths, the revive order open
		int busiest = 0;
		for (int i = 0; i < static_cast<int>(fights.size()); i++) { if (fights[i]->SquadDowns > fights[busiest]->SquadDowns) { busiest = i; } }
		Ui::S().Selected = busiest;
		Ui::S().AllRounds = false;
		Ui::ForcedOpen = Ui::kForceReviveOrder;
		for (int tab : {Ui::T_Round, Ui::T_Deaths})
		{
			Ui::ForcedTab = tab;
			for (int n = 0; n < 3; n++) { Frame(); }
			double sum = 0, worst = 0;
			for (int n = 0; n < 30; n++) { Frame(); sum += s_RenderMs; worst = std::max(worst, s_RenderMs); }
			std::printf("%s, round %d %5.2f ms per frame (worst %.2f), %d downs\n", tab == Ui::T_Round ? "round" : "deaths", busiest + 1, sum / 30, worst, fights[busiest]->SquadDowns);
		}
		Ui::ForcedOpen = -1;
		Ui::S().Selected = -1;
		Ui::ShowWindow = false;
		Ui::ShowMini = true;
		double sum = 0;
		for (int n = 0; n < 30; n++) { Frame(); sum += s_RenderMs; }
		std::printf("%-22s %6.2f ms per frame\n", "summary window", sum / 30);
		ImGui_ImplDX11_Shutdown();
		ImGui::DestroyContext();
		return 0;
	}
	auto save = [&](const char* aName, bool aBottom = false)
	{
		for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) { ImGui::SetScrollY(w, 0); }
		for (int n = 0; n < 4; n++) { Frame(); }
		if (aBottom)
		{
			for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) { if (w->ScrollMax.y > 0) { ImGui::SetScrollY(w, w->ScrollMax.y); } }
			for (int n = 0; n < 3; n++) { Frame(); }
		}
		std::string path = out + "/" + aName + ".png";
		std::wstring wpath(path.begin(), path.end());
		std::printf("%s %s\n", SavePng(wpath) ? "saved" : "FAILED", path.c_str());
	};
	struct Shot { const char* Name; int Tab; int Metric; int Open; };
	const Shot shots[] = {{"you", Ui::T_You, -1, -1}, {"you_open", Ui::T_You, -1, 0}, {"compare_role", Ui::T_Compare, -1, -1}, {"compare_heal", Ui::T_Compare, 0, -1},
		{"compare_dmg", Ui::T_Compare, 2, -1}, {"squad", Ui::T_Squad, -1, -1}, {"tonight", Ui::T_Tonight, -1, -1}, {"deaths", Ui::T_Deaths, -1, -1}, {"round", Ui::T_Round, -1, -1}};
	for (const Shot& s : shots)
	{
		Ui::ForcedTab = s.Tab;
		Ui::ForcedMetric = s.Metric;
		Ui::ForcedOpen = s.Open;
		save(s.Name);
	}
	Ui::ForcedMetric = -1;
	Ui::ForcedOpen = -1;
	// Deaths and Round scrolled to the end (the revive order open)
	Ui::ForcedTab = Ui::T_Deaths;
	Ui::ForcedOpen = Ui::kForceReviveOrder;
	save("deaths_bottom", true);
	Ui::ForcedOpen = -1;
	Ui::ForcedTab = Ui::T_Round;
	save("round_bottom", true);
	// Two skills marked: the round's two most damaging skills, then the round's biggest spike of ours broken down
	{
		std::map<int32_t, double> by;
		for (const auto& p : shown.Players) { for (const auto& h : p.HitsOut) { by[h.Skill] += h.Damage; } }
		std::vector<std::pair<double, int32_t>> order;
		for (auto& [sk, d] : by) { if (sk > 0) { order.push_back({d, sk}); } }
		std::sort(order.rbegin(), order.rend());
		for (size_t i = 0; i < order.size() && i < 3; i++) { Ui::S().Picked.push_back(order[i].second); }
		save("round_marked");
		int64_t best = -1;
		double most = -1;
		for (int64_t t : shown.OurSpikesMs)
		{
			size_t sec = static_cast<size_t>(t / 1000);
			double d = sec < shown.ToPlayersPerS.size() ? double(shown.ToPlayersPerS[sec]) : 0.0;
			if (d > most) { most = d; best = t; }
		}
		if (best >= 0)
		{
			Ui::S().SpikeOpen = best;
			Ui::S().SpikeStamp = shown.Stamp;
			save("spike");
			save("spike_bottom", true);
			Ui::S().SpikeOpen = -1;
		}
		Ui::S().RoundView = 1;
		save("round_stability");
		Ui::S().RoundView = 0;
	}
	// Compare with two other players picked (the round's first two who aren't you)
	{
		std::vector<std::string> others;
		for (const auto& p : shown.Players) { if (!p.Pov) { others.push_back(p.Account); } }
		if (others.size() >= 2)
		{
			Ui::ForcedTab = Ui::T_Compare;
			Ui::S().CompareLeft = others[0];
			Ui::S().CompareRight = others[1];
			save("compare_pair");
			Ui::S().CompareLeft.clear();
			Ui::S().CompareRight.clear();
		}
	}
	// The small summary window on its own
	Ui::ShowWindow = false;
	Ui::ShowMini = true;
	save("summary");
	ImGui_ImplDX11_Shutdown();
	ImGui::DestroyContext();
	return 0;
}
