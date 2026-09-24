// Renders the addon's real window offscreen from real logs and saves a PNG per tab, with ImGui's asserts on
// (they are off in the release DLL, where a mistake would take the game down instead).
//
//   build/release/fr_uishot.exe --out <dir> <log.zevtc> [more logs ...]
//
// Exit code 1 if a render failed or an ImGui assert fired.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include "Analysis.h"
#include "Session.h"
#include "Ui.h"

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

	void Frame()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(kWidth, kHeight);
		io.DeltaTime = 1.0f / 60.0f;
		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(980, 860), ImGuiCond_Always);
		Ui::Render();
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
	std::string out = ".";
	std::vector<Session::FightPtr> fights;
	for (int i = 1; i < argc; i++)
	{
		if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) { out = argv[++i]; continue; }
		try { fights.push_back(std::make_shared<Analysis::Fight>(Analysis::Analyse(argv[i]))); }
		catch (const std::exception& e) { std::printf("%s: %s\n", argv[i], e.what()); return 1; }
	}
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (!CreateDevice()) { std::printf("no D3D11 device\n"); return 1; }
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGui_ImplDX11_Init(s_Device, s_Context);

	Session::SetForTest(fights);
	Ui::ShowWindow = true;
	struct Shot { const char* Name; int Tab; int Metric; int Open; };
	const Shot shots[] = {{"review", 0, -1, -1}, {"review_open", 0, -1, 0}, {"review_skill", 0, -1, 3}, {"compare_role", 1, -1, -1}, {"compare_heal", 1, 0, -1},
		{"compare_dmg", 1, 2, -1}, {"compare_cleanses", 1, 5, -1}, {"squad", 2, -1, -1}, {"fight", 3, -1, -1}};
	for (const Shot& s : shots)
	{
		Ui::ForcedTab = s.Tab;
		Ui::ForcedMetric = s.Metric;
		Ui::ForcedOpen = s.Open;
		for (int n = 0; n < 4; n++) { Frame(); }
		std::string path = out + "/" + s.Name + ".png";
		std::wstring wpath(path.begin(), path.end());
		std::printf("%s %s\n", SavePng(wpath) ? "saved" : "FAILED", path.c_str());
	}
	ImGui_ImplDX11_Shutdown();
	ImGui::DestroyContext();
	return 0;
}
