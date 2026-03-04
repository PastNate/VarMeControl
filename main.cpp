#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <map>      // Fixes: 'map' is not a member of std
#include <thread>
#include <chrono>
#include <iomanip>   // Fixes: 'setprecision' is not a member of std

// Include hidapi
#include "hidapi.h"

// Include ViGEmClient
// You might need to adjust this path based on ViGEmClient's header structure
#include "Client.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <d3d11.h>
#include <tchar.h>

struct Binding {
    std::string name;
    unsigned char keyID = 0;
    bool isWaiting = false;
    float currentPercent = 0.0f;
};

// A list of all X360 buttons we want to map
std::vector<Binding> myBindings = {
    {"Left Stick Up (W)", 26},
    {"Left Stick Down (S)", 22},
    {"Button A", 0},
    {"Button B", 0}
};

// Data for DX11
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- YOUR MAIN ---
int main(int, char**) {
    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"VarMeControl", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"V-Muse Analog Pro", WS_OVERLAPPEDWINDOW, 100, 100, 450, 600, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // --- INITIALIZE YOUR HID/VIGEM HERE ---
    // (Copy your hid_init and vigem_alloc logic here)
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // --- 1. HID READ LOGIC ---
        unsigned char report[64];
        int res = hid_read(dev_handle, report, sizeof(report));

        XUSB_REPORT xinput_report;
        XUSB_REPORT_INIT(&xinput_report);

        // Example: Map the first 4 bindings to Left Stick
        // Note: We subtract "Down" from "Up" to get the Y axis
        float axisY = 0.0f;
        float axisX = 0.0f;

        if (myBindings[0].keyID != 0) axisY += myBindings[0].currentPercent; // Up
        if (myBindings[1].keyID != 0) axisY -= myBindings[1].currentPercent; // Down
        if (myBindings[2].keyID != 0) axisX -= myBindings[2].currentPercent; // Left
        if (myBindings[3].keyID != 0) axisX += myBindings[3].currentPercent; // Right

        xinput_report.sThumbLX = static_cast<SHORT>(axisX * 32767);
        xinput_report.sThumbLY = static_cast<SHORT>(axisY * 32767);

        // Send to virtual controller
        vigem_target_x360_update(client, pad, xinput_report);

        if (res > 0) {
            unsigned char incomingKeyID = report[3];
            int rawVal = (static_cast<int>(report[4]) << 8) | static_cast<int>(report[5]);

            bool bindingCaptured = false;
            for (auto& bind : myBindings) {
                // --- REBINDING LOGIC ---
                if (bind.isWaiting && incomingKeyID != 0) {
                    bind.keyID = incomingKeyID;
                    bind.isWaiting = false;
                    bindingCaptured = true;
                }

                // --- CALCULATION LOGIC ---
                if (bind.keyID == incomingKeyID && incomingKeyID != 0) {
                    // Simplified percent for the GUI bars (0.0 to 1.0)
                    // You can re-insert your min/max calibration math here
                    bind.currentPercent = static_cast<float>(rawVal) / 8192.0f;
                }
            }
        } else {
            // If no keys are pressed, fade the bars to 0
            for (auto& bind : myBindings) {
                bind.currentPercent *= 0.9f;
                if (bind.currentPercent < 0.01f) bind.currentPercent = 0.0f;
            }
        }

        // --- 2. START IMGUI FRAME ---
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // --- 3. THE GUI WINDOW ---
        ImGui::Begin("Controller Remapper");

        ImGui::Text("Click a button to rebind:");
        ImGui::Separator();

        for (auto& bind : myBindings) {
            ImGui::PushID(bind.name.c_str());

            if (ImGui::Button("X")) {
                bind.keyID = 0;
                bind.isWaiting = false;
            }
            ImGui::SameLine();
            // ... followed by your existing Button and ProgressBar code

            ImGui::Text("%-20s", bind.name.c_str());
            ImGui::SameLine();

            std::string btnText = bind.isWaiting ? "???" : (bind.keyID == 0 ? "None" : std::to_string(bind.keyID));
            if (ImGui::Button(btnText.c_str(), ImVec2(100, 0))) {
                // Cancel others, set this one to wait
                for (auto& b : myBindings) b.isWaiting = false;
                bind.isWaiting = true;
            }

            ImGui::SameLine();
            ImGui::ProgressBar(bind.currentPercent, ImVec2(100, 0));

            ImGui::PopID();
        }

        if (ImGui::Button("Save Config", ImVec2(-1, 40))) {
            // Future: Save to file
        }

        ImGui::End();

        // --- 4. RENDERING ---
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.1f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // Present with vsync
    }

    // --- CLEANUP ---
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// --- BOILERPLATE WIN32/D3D FUNCTIONS ---
// (Paste these exactly as is from the ImGui 'main.cpp' example for Win32+DX11)