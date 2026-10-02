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
#include <cfloat>
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
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <d3d11.h>
#include <wincodec.h>

#include <winhttp.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include "Analysis.h"
#include "Icons.h"
#include "nexus/Nexus.h"
#include "Session.h"
#include "SkillIcons.h"
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

	// aX0..aY1: the part of the frame to save (--crop: the windows drawn), else all of it
	bool SavePng(const std::wstring& aPath, int aX0 = 0, int aY0 = 0, int aX1 = kWidth, int aY1 = kHeight)
	{
		s_Context->CopyResource(s_Staging, s_Target);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(s_Context->Map(s_Staging, 0, D3D11_MAP_READ, 0, &mapped))) { return false; }
		const int kW = aX1 - aX0, kH = aY1 - aY0; // the saved part
		std::vector<unsigned char> pixels(static_cast<size_t>(kW) * kH * 4);
		for (int y = 0; y < kH; y++)
		{
			std::memcpy(pixels.data() + static_cast<size_t>(y) * kW * 4,
				static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y + aY0) * mapped.RowPitch + static_cast<size_t>(aX0) * 4, kW * 4);
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
		if (SUCCEEDED(factory->CreateBitmapFromMemory(kW, kH, GUID_WICPixelFormat32bppRGBA, kW * 4,
				static_cast<UINT>(pixels.size()), pixels.data(), &bitmap)) &&
			SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
			SUCCEEDED(converter->Initialize(bitmap, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
				WICBitmapPaletteTypeCustom)) &&
			SUCCEEDED(factory->CreateStream(&stream)) &&
			SUCCEEDED(stream->InitializeFromFilename(aPath.c_str(), GENERIC_WRITE)) &&
			SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
			SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
			SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
			SUCCEEDED(frame->SetSize(kW, kH)))
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

	// --icons: the game's icons, as Nexus loads them in game (here from the same links, through WinHTTP and WIC)
	bool s_Icons = false;
	std::map<std::string, Texture_t> s_Textures; // identifier -> texture (Resource null: it failed, don't retry)

	Texture_t* TextureFromBytes(const std::string& aId, const void* aData, size_t aSize)
	{
		Texture_t& t = s_Textures[aId];
		IWICImagingFactory* factory = nullptr;
		if (!aSize || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { return nullptr; }
		IWICStream* stream = nullptr;
		IWICBitmapDecoder* decoder = nullptr;
		IWICBitmapFrameDecode* frame = nullptr;
		IWICFormatConverter* converter = nullptr;
		UINT w = 0, h = 0;
		std::vector<BYTE> pixels;
		if (SUCCEEDED(factory->CreateStream(&stream)) &&
			SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(aData)), static_cast<DWORD>(aSize))) &&
			SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
			SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
			SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
			SUCCEEDED(converter->GetSize(&w, &h)))
		{
			pixels.resize(static_cast<size_t>(w) * h * 4);
			if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()))) { pixels.clear(); }
		}
		if (converter) { converter->Release(); }
		if (frame) { frame->Release(); }
		if (decoder) { decoder->Release(); }
		if (stream) { stream->Release(); }
		factory->Release();
		if (pixels.empty()) { return nullptr; }
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{pixels.data(), w * 4, 0};
		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* view = nullptr;
		if (FAILED(s_Device->CreateTexture2D(&desc, &init, &texture)) || FAILED(s_Device->CreateShaderResourceView(texture, nullptr, &view)))
		{
			if (texture) { texture->Release(); }
			return nullptr;
		}
		texture->Release(); // the view keeps it
		t = {w, h, view};
		return &t;
	}

	Texture_t* FromMemory(const char* aId, void* aData, uint64_t aSize)
	{
		if (auto it = s_Textures.find(aId); it != s_Textures.end()) { return it->second.Resource ? &it->second : nullptr; }
		return TextureFromBytes(aId, aData, static_cast<size_t>(aSize));
	}

	Texture_t* FromUrl(const char* aId, const char* aRemote, const char* aEndpoint)
	{
		if (auto it = s_Textures.find(aId); it != s_Textures.end()) { return it->second.Resource ? &it->second : nullptr; }
		std::string remote = aRemote;
		if (size_t scheme = remote.find("://"); scheme != std::string::npos) { remote = remote.substr(scheme + 3); }
		std::wstring host(remote.begin(), remote.end()), path(aEndpoint, aEndpoint + std::strlen(aEndpoint));
		std::string body;
		HINTERNET session = WinHttpOpen(L"FightReview-uishot/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		HINTERNET connect = session ? WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
		HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
		if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
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
		if (session) { WinHttpCloseHandle(session); }
		return TextureFromBytes(aId, body.data(), body.size());
	}

	double s_RenderMs = 0; // CPU time of the last Ui::Render()
	ImVec2 s_Mouse(-FLT_MAX, -FLT_MAX); // --hover: where the mouse rests, to render a tooltip

	void Frame()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(kWidth, kHeight);
		io.DeltaTime = s_Icons ? 1.1f : 1.0f / 60.0f; // --icons: a second a frame (an icon is asked for once a second)
		io.MousePos = s_Mouse;
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

// --fake: made-up character and account names in place of every squad member's, the same across rounds (pictures for
// the README; logs carry real players' names). Enemies are anonymous in the log already.
void FakeNames(Analysis::Fight& f)
{
	static const char* const kNames[] = {"Aldra Venn", "Borik Stoneglass", "Cael Morrow", "Dessa Quill", "Edric Thane", "Fennel Ash",
		"Garrow Pike", "Hessa Lund", "Ivo Marsh", "Jora Kell", "Kade Wren", "Lysa Brant", "Maro Fell", "Nessa Coil", "Orin Vale",
		"Pell Duskar", "Quen Harrow", "Rhea Solm", "Saro Finch", "Tamsin Rook", "Ulla Grey", "Varn Hollis", "Wynn Carde",
		"Yara Moss", "Zeb Thorn", "Ansel Crane", "Brisa Lowe", "Corin Ashby", "Dain Rell", "Elsa Marrow", "Fyn Calder",
		"Gwen Tallis", "Holt Breck", "Isla Venn", "Jarek Stroud", "Kira Selm", "Loric Dane", "Mina Corr", "Nils Barrow",
		"Odra Pine", "Piers Lark", "Rowan Teague", "Sefa Lind", "Tobin Shale", "Una Brisk", "Vesna Holt", "Wren Ashcombe"};
	static std::map<std::string, int> s_Index; // account -> name
	auto fakeOf = [](const std::string& aAccount) -> int
	{
		auto it = s_Index.find(aAccount);
		if (it != s_Index.end()) { return it->second; }
		int n = static_cast<int>(s_Index.size());
		s_Index[aAccount] = n;
		return n;
	};
	auto account = [&](const std::string& aAccount)
	{
		int n = fakeOf(aAccount);
		return std::string("Player.") + std::to_string(1000 + n * 37 % 9000);
	};
	for (auto& p : f.Players)
	{
		int n = fakeOf(p.Account);
		const int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
		p.Name = std::string(kNames[n % kCount]) + (n >= kCount ? " " + std::to_string(n / kCount + 1) : std::string());
		p.Account = account(p.Account);
	}
	for (auto& g : f.LearnedGivers) { g.Account = account(g.Account); }
	std::map<std::pair<std::string, int32_t>, Analysis::BoonEvidence> byPlayer;
	for (auto& [key, ev] : f.OwnEvidence.ByPlayer) { byPlayer[{account(key.first), key.second}] = ev; }
	f.OwnEvidence.ByPlayer.swap(byPlayer);
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
		while (quiet < 12)
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
	std::vector<ImVec2> hovers; // --hover <x,y>: the Summary tab with the mouse resting there (summary_hover_N.png)
	std::string ini;            // --ini <arcdps.ini>: also render the empty window's first-run check against it (first_run.png)
	std::string settings;       // --settings <settings.txt>: the addon's settings (the summary window's style); saved back to it
	std::vector<Session::FightPtr> fights;
	bool memory = false;                // --memory: what the rounds cost in memory and analysis time, then stop
	bool fake = false;                  // --fake: made-up names for every squad member (pictures to publish); before the logs
	bool crop = false;                  // --crop: save only the windows drawn, not the whole frame
	double analyseMs = 0, analyseWorst = 0;
	for (int i = 1; i < argc; i++)
	{
		if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) { out = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--time") == 0) { timing = true; continue; }
		if (std::strcmp(argv[i], "--jobs") == 0) { jobs = true; continue; }
		if (std::strcmp(argv[i], "--as") == 0 && i + 1 < argc) { as = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--round") == 0 && i + 1 < argc) { round = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--down") == 0 && i + 1 < argc) { down = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) { scale = static_cast<float>(std::atof(argv[++i])); continue; }
		if (std::strcmp(argv[i], "--ini") == 0 && i + 1 < argc) { ini = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--settings") == 0 && i + 1 < argc) { settings = argv[++i]; continue; }
		if (std::strcmp(argv[i], "--hover") == 0 && i + 1 < argc)
		{
			float hx = 0, hy = 0;
			if (std::sscanf(argv[++i], "%f,%f", &hx, &hy) == 2) { hovers.push_back(ImVec2(hx, hy)); }
			continue;
		}
		if (std::strcmp(argv[i], "--memory") == 0) { memory = true; continue; }
		if (std::strcmp(argv[i], "--fake") == 0) { fake = true; continue; }
		if (std::strcmp(argv[i], "--crop") == 0) { crop = true; continue; }
		if (std::strcmp(argv[i], "--icons") == 0) { s_Icons = true; continue; }
		auto t0 = std::chrono::steady_clock::now();
		try
		{
			Analysis::Fight f = Analysis::Analyse(argv[i]);
			if (fake) { FakeNames(f); }
			fights.push_back(std::make_shared<Analysis::Fight>(std::move(f)));
		}
		catch (const std::exception& e) { std::printf("%s: %s\n", argv[i], e.what()); return 1; }
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		analyseMs += ms; analyseWorst = std::max(analyseWorst, ms);
	}
	if (memory)
	{
		// What a night costs in memory (the addon keeps every round), and where it goes
		PROCESS_MEMORY_COUNTERS_EX pmc{};
		GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
		std::map<std::string, double> parts;
		auto add = [&](const char* aName, size_t aBytes) { parts[aName] += double(aBytes); };
		for (const auto& f : fights)
		{
			for (const auto& p : f->Players)
			{
				add("hits in and out", (p.HitsIn.capacity() + p.HitsOut.capacity() + p.EvadedIn.capacity()) * sizeof(Analysis::Player::TakenHit));
				add("positions", p.Pos.capacity() * sizeof(Analysis::Player::Point));
				add("health", p.Hp.capacity() * sizeof(std::pair<int32_t, int32_t>));
				add("heals in", p.HealsIn.capacity() * sizeof(std::pair<int32_t, int32_t>));
				size_t boonOn = 0;
				for (const auto& b : p.BoonOn) { boonOn += b.capacity(); }
				add("boon spans", boonOn * sizeof(std::pair<int32_t, int32_t>));
				size_t skills = 0;
				for (const auto& [id, r] : p.Skills) { skills += sizeof(Analysis::SkillRow) + 48 + r.CastMs.capacity() * 4; }
				add("skill rows", skills);
				add("per second", (p.DamagePerS.capacity() + p.HealPerS.capacity()) * 4);
				add("other boon moments", p.OtherBoonMs.capacity() * sizeof(std::pair<int32_t, int>));
				size_t gives = 0;
				for (const auto& g : p.StabGives) { gives += sizeof(g) + g.Targets.capacity() * sizeof(int); }
				add("stability gives", gives);
				add("strips, CC, dodges, casts", p.StripsIn.capacity() * sizeof(Analysis::Player::StripHit) + p.CcIn.capacity() * sizeof(Analysis::Player::CcHit) +
					(p.CcMs.capacity() + p.DodgeMs.capacity() + p.CleanseMs.capacity() + p.StripMs.capacity() + p.TeleportMs.capacity()) * 4);
			}
			for (const auto& e : f->Enemies) { add("enemies (positions, health)", e.Pos.capacity() * sizeof(Analysis::Player::Point) + e.Hp.capacity() * 8); }
			add("skill names", f->SkillNames.size() * 64.0);
			size_t ev = 0;
			for (auto& [k, v] : f->OwnEvidence.ByPlayer) { ev += sizeof(v) + k.first.size() + 64; }
			ev += f->OwnEvidence.ByProfession.size() * (sizeof(Analysis::BoonEvidence) + 48);
			add("boon evidence", ev);
		}
		double total = 0;
		for (auto& [k, v] : parts) { total += v; }
		std::printf("%zu rounds: analysis %.0f ms in all, %.0f ms the slowest; private memory %.1f MB\n", fights.size(), analyseMs, analyseWorst, pmc.PrivateUsage / 1048576.0);
		std::printf("counted in the rounds' data: %.1f MB\n", total / 1048576.0);
		std::vector<std::pair<double, std::string>> sorted;
		for (auto& [k, v] : parts) { sorted.push_back({v, k}); }
		std::sort(sorted.rbegin(), sorted.rend());
		for (auto& [v, k] : sorted) { std::printf("  %7.2f MB  %s\n", v / 1048576.0, k.c_str()); }
		return 0;
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

	static AddonAPI_t api{}; // --icons: only the two texture calls the addon makes
	if (s_Icons)
	{
		api.Textures_GetOrCreateFromMemory = FromMemory;
		api.Textures_GetOrCreateFromURL = FromUrl;
		Icons::Init(&api);
		SkillIcons::Init(&api, std::filesystem::path(out) / "skill_icons.txt");
	}
	Session::SetForTest(fights);
	if (!settings.empty()) { Ui::LoadSettings(settings); }
	Ui::ShowWindow = true;
	Ui::ShowMini = false; // on by default in game; here it would take the main window's place (its own shot below)
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
		const Case cases[] = {{"summary", Ui::T_Summary, false}, {"you, this round", Ui::T_You, false}, {"you, tonight", Ui::T_You, true}, {"deaths", Ui::T_Deaths, false},
			{"compare, this round", Ui::T_Compare, false}, {"compare, tonight", Ui::T_Compare, true}, {"round", Ui::T_Round, false},
			{"squad, this round", Ui::T_Squad, false}, {"squad, tonight", Ui::T_Squad, true}};
		for (const Case& k : cases)
		{
			Ui::ForcedTab = k.Tab;
			Ui::S().YouTonight = Ui::S().CompareTonight = Ui::S().SquadTonight = k.AllRounds;
			// the first frames build the tab's caches (a hitch when switching tab or when a round arrives)
			double first = 0;
			for (int n = 0; n < 3; n++) { Frame(); first = std::max(first, s_RenderMs); }
			double sum = 0, worst = 0;
			const int kFrames = 30;
			for (int n = 0; n < kFrames; n++) { Frame(); sum += s_RenderMs; worst = std::max(worst, s_RenderMs); }
			std::printf("%-22s %6.2f ms per frame (worst %.2f), first frames up to %.1f ms\n", k.Name, sum / kFrames, worst, first);
		}
		// A new round arriving at the end of the night: the frame it shows up on (Summary, then You) rebuilds what
		// depends on the rounds; the rest of the night's work is kept
		if (fights.size() >= 2)
		{
			for (int tab : {Ui::T_Summary, Ui::T_You})
			{
				Ui::ForcedTab = tab;
				Ui::S().YouTonight = false;
				Session::SetForTest(std::vector<Session::FightPtr>(fights.begin(), fights.end() - 1));
				for (int n = 0; n < 5; n++) { Frame(); }
				Session::SetForTest(fights);
				double first = 0;
				for (int n = 0; n < 3; n++) { Frame(); first = std::max(first, s_RenderMs); }
				std::printf("%-22s first frames up to %.1f ms when round %zu arrives\n", tab == Ui::T_Summary ? "summary, new round" : "you, new round", first, fights.size());
			}
		}
		// The round with the most downs: Round and Deaths, the revive order open
		int busiest = 0;
		for (int i = 0; i < static_cast<int>(fights.size()); i++) { if (fights[i]->SquadDowns > fights[busiest]->SquadDowns) { busiest = i; } }
		Ui::S().Selected = busiest;
		Ui::S().YouTonight = Ui::S().CompareTonight = Ui::S().SquadTonight = false;
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
		// --icons: frames until the icons asked for have come (the GW2 API's icon links load on a worker thread)
		for (int n = 0; s_Icons && n < 12; n++) { ::Sleep(120); Frame(); }
		if (aBottom)
		{
			for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) { if (w->ScrollMax.y > 0) { ImGui::SetScrollY(w, w->ScrollMax.y); } }
			for (int n = 0; n < 3; n++) { Frame(); }
		}
		std::string path = out + "/" + aName + ".png";
		std::wstring wpath(path.begin(), path.end());
		// --crop: the windows drawn this frame, tooltips too
		float x0 = static_cast<float>(kWidth), y0 = static_cast<float>(kHeight), x1 = 0, y1 = 0;
		for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
		{
			if (!w->WasActive || w->Hidden) { continue; }
			x0 = std::min(x0, w->Pos.x); y0 = std::min(y0, w->Pos.y);
			x1 = std::max(x1, w->Pos.x + w->Size.x); y1 = std::max(y1, w->Pos.y + w->Size.y);
		}
		bool cut = crop && x1 > x0 && y1 > y0;
		auto clampTo = [](float v, int hi) { return std::clamp(static_cast<int>(v), 0, hi); };
		bool ok = cut ? SavePng(wpath, clampTo(x0, kWidth), clampTo(y0, kHeight), clampTo(x1 + 1, kWidth), clampTo(y1 + 1, kHeight)) : SavePng(wpath);
		std::printf("%s %s\n", ok ? "saved" : "FAILED", path.c_str());
	};
	struct Shot { const char* Name; int Tab; int Metric; int Open; };
	const Shot shots[] = {{"summary_tab", Ui::T_Summary, -1, -1}, {"you", Ui::T_You, -1, -1}, {"you_open", Ui::T_You, -1, 0}, {"compare_role", Ui::T_Compare, -1, -1}, {"compare_heal", Ui::T_Compare, 0, -1},
		{"compare_dmg", Ui::T_Compare, 2, -1}, {"squad", Ui::T_Squad, -1, -1}, {"tonight", Ui::T_You, -1, -2}, {"deaths", Ui::T_Deaths, -1, -1}, {"round", Ui::T_Round, -1, -1}};
	for (const Shot& s : shots)
	{
		Ui::ForcedTab = s.Tab;
		Ui::ForcedMetric = s.Metric;
		Ui::ForcedOpen = s.Open == -2 ? -1 : s.Open;
		Ui::S().YouTonight = s.Open == -2; // -2: You on Tonight
		save(s.Name);
		Ui::S().YouTonight = false;
	}
	Ui::ForcedMetric = -1;
	Ui::ForcedOpen = -1;
	for (size_t i = 0; i < hovers.size(); i++)
	{
		Ui::ForcedTab = Ui::T_Summary;
		s_Mouse = hovers[i];
		save(("summary_hover_" + std::to_string(i + 1)).c_str());
	}
	s_Mouse = ImVec2(-FLT_MAX, -FLT_MAX);
	// Deaths' revives view (the revive order open), and Round scrolled to the end
	Ui::ForcedTab = Ui::T_Deaths;
	Ui::ForcedOpen = Ui::kForceReviveOrder;
	Ui::S().DeathFilter = 3;
	save("revives");
	Ui::S().DeathFilter = 0;
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
		// the enemy spike with the most of our damage taken, broken down; and two of its skills marked on the round
		{
			int64_t worst = -1;
			double most = -1;
			for (int64_t t : shown.TheirSpikesMs)
			{
				size_t sec = static_cast<size_t>(t / 1000);
				double d = sec < shown.InPerS.size() ? double(shown.InPerS[sec]) : 0.0;
				if (d > most) { most = d; worst = t; }
			}
			if (worst >= 0)
			{
				Ui::S().SpikeOpen = worst;
				Ui::S().SpikeEnemy = true;
				Ui::S().SpikeStamp = shown.Stamp;
				save("enemy_spike");
				Ui::S().SpikeOpen = -1;
				Ui::S().SpikeEnemy = false;
				std::map<int32_t, double> byIn;
				for (const auto& p : shown.Players) { for (const auto& h : p.HitsIn) { if (h.Skill > 0) { byIn[h.Skill] += h.Damage; } } }
				std::vector<std::pair<double, int32_t>> top;
				for (auto& [sk, d] : byIn) { top.push_back({d, sk}); }
				std::sort(top.rbegin(), top.rend());
				for (size_t i = 0; i < top.size() && i < 2; i++) { Ui::S().PickedEnemy.push_back(top[i].second); }
				save("round_marked_both");
				// the same skills on the enemy spike's own graph
				Ui::S().SpikeOpen = worst;
				Ui::S().SpikeEnemy = true;
				save("enemy_spike_marked");
				save("enemy_spike_marked_bottom", true);
				Ui::S().SpikeOpen = -1;
				Ui::S().SpikeEnemy = false;
				Ui::S().PickedEnemy.clear();
			}
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
	// No rounds yet: the first-run check against the given arcdps.ini
	if (!ini.empty())
	{
		Ui::ShowMini = false;
		Ui::ShowWindow = true;
		Ui::ForcedTab = -1;
		Ui::SetArcdpsIni(ini);
		Session::SetForTest({});
		save("first_run");
	}
	if (s_Icons) { SkillIcons::Shutdown(); }
	ImGui_ImplDX11_Shutdown();
	ImGui::DestroyContext();
	return 0;
}
