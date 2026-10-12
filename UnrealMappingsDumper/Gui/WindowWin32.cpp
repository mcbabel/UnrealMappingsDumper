#include "../pch.h"

#include "Window.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	HWND MainWindow = nullptr;
	WNDCLASSEXW WindowClass{};

	ID3D11Device* Device = nullptr;
	ID3D11DeviceContext* DeviceContext = nullptr;
	IDXGISwapChain* SwapChain = nullptr;
	ID3D11RenderTargetView* RenderTarget = nullptr;

	UINT ResizeWidth = 0;
	UINT ResizeHeight = 0;
	bool bClosed = false;
	bool bOccluded = false;

	void CreateRenderTarget()
	{
		ID3D11Texture2D* BackBuffer = nullptr;
		SwapChain->GetBuffer(0, IID_PPV_ARGS(&BackBuffer));

		if (BackBuffer)
		{
			Device->CreateRenderTargetView(BackBuffer, nullptr, &RenderTarget);
			BackBuffer->Release();
		}
	}

	void CleanupRenderTarget()
	{
		if (RenderTarget)
		{
			RenderTarget->Release();
			RenderTarget = nullptr;
		}
	}

	bool CreateDevice(HWND Window)
	{
		DXGI_SWAP_CHAIN_DESC Desc{};
		Desc.BufferCount = 2;
		Desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		Desc.BufferDesc.RefreshRate.Numerator = 60;
		Desc.BufferDesc.RefreshRate.Denominator = 1;
		Desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		Desc.OutputWindow = Window;
		Desc.SampleDesc.Count = 1;
		Desc.Windowed = TRUE;
		Desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		const D3D_FEATURE_LEVEL Levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL Level;

		auto Result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, Levels, 2, D3D11_SDK_VERSION, &Desc, &SwapChain, &Device, &Level, &DeviceContext);

		if (Result != S_OK)
			Result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, Levels, 2, D3D11_SDK_VERSION, &Desc, &SwapChain, &Device, &Level, &DeviceContext);

		if (Result != S_OK)
			return false;

		CreateRenderTarget();
		return true;
	}

	void CleanupDevice()
	{
		CleanupRenderTarget();

		if (SwapChain) { SwapChain->Release(); SwapChain = nullptr; }
		if (DeviceContext) { DeviceContext->Release(); DeviceContext = nullptr; }
		if (Device) { Device->Release(); Device = nullptr; }
	}

	LRESULT WINAPI WndProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(Window, Message, WParam, LParam))
			return TRUE;

		switch (Message)
		{
		case WM_SIZE:
			if (WParam != SIZE_MINIMIZED)
			{
				ResizeWidth = LOWORD(LParam);
				ResizeHeight = HIWORD(LParam);
			}
			return 0;
		case WM_GETMINMAXINFO:
		{
			auto Info = reinterpret_cast<MINMAXINFO*>(LParam);
			Info->ptMinTrackSize.x = 640;
			Info->ptMinTrackSize.y = 480;
			return 0;
		}
		case WM_SYSCOMMAND:
			if ((WParam & 0xfff0) == SC_KEYMENU)
				return 0;
			break;
		case WM_DESTROY:
			bClosed = true;
			PostQuitMessage(0);
			return 0;
		}

		return DefWindowProcW(Window, Message, WParam, LParam);
	}

	std::wstring ToWide(const char* Utf8)
	{
		auto Size = MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, nullptr, 0);
		std::wstring Ret(Size > 0 ? Size - 1 : 0, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, Ret.data(), Size);
		return Ret;
	}
}

bool Gui::Window::Create(const char* Title, int Width, int Height)
{
	ImGui_ImplWin32_EnableDpiAwareness();

	auto Scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

	WindowClass = { sizeof(WindowClass), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"UnrealMappingsDumper", nullptr };
	WindowClass.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
	RegisterClassExW(&WindowClass);

	MainWindow = CreateWindowW(WindowClass.lpszClassName, ToWide(Title).c_str(), WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(Width * Scale), static_cast<int>(Height * Scale), nullptr, nullptr, WindowClass.hInstance, nullptr);

	if (!MainWindow || !CreateDevice(MainWindow))
	{
		CleanupDevice();

		if (MainWindow)
			DestroyWindow(MainWindow);

		UnregisterClassW(WindowClass.lpszClassName, WindowClass.hInstance);
		return false;
	}

	ShowWindow(MainWindow, SW_SHOWDEFAULT);
	UpdateWindow(MainWindow);

	ImGui::CreateContext();

	ImGui_ImplWin32_Init(MainWindow);
	ImGui_ImplDX11_Init(Device, DeviceContext);

	return true;
}

bool Gui::Window::PumpEvents(bool bAnimating)
{
	if (!bAnimating)
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);

	MSG Message;

	while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&Message);
		DispatchMessageW(&Message);

		if (Message.message == WM_QUIT)
			bClosed = true;
	}

	if (bClosed)
		return false;

	if (bOccluded && SwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
	{
		Sleep(10);
		return PumpEvents(bAnimating);
	}

	bOccluded = false;

	if (ResizeWidth != 0 && ResizeHeight != 0)
	{
		CleanupRenderTarget();
		SwapChain->ResizeBuffers(0, ResizeWidth, ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
		ResizeWidth = ResizeHeight = 0;
		CreateRenderTarget();
	}

	return true;
}

void Gui::Window::BeginFrame()
{
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

void Gui::Window::EndFrame(const float ClearColor[4])
{
	ImGui::Render();

	DeviceContext->OMSetRenderTargets(1, &RenderTarget, nullptr);
	DeviceContext->ClearRenderTargetView(RenderTarget, ClearColor);

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	bOccluded = SwapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
}

void Gui::Window::Destroy()
{
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDevice();
	DestroyWindow(MainWindow);
	UnregisterClassW(WindowClass.lpszClassName, WindowClass.hInstance);
	MainWindow = nullptr;
}

float Gui::Window::GetContentScale()
{
	return std::max(1.0f, ImGui_ImplWin32_GetDpiScaleForHwnd(MainWindow));
}
