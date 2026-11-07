#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <iomanip>   // Fixes: 'setprecision' is not a member of std

#include "hidapi.h"

// Include ViGEmClient
#include "Client.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <d3d11.h>
#include <tchar.h>
#include <gdiplus.h>

struct Binding {
    std::string name;
    unsigned char keyID = 0;
    bool isWaiting = false;
    float currentPercent = 0.0f;
    int minRaw = 100; // Typical resting value
    int maxRaw = 800; // Initial guess, will auto-adjust
};

struct CsvInputRow {
    std::string keyLabel;
    int number = 0;
    std::string control;
};

// --- Constants ---
const unsigned short VENDOR_ID = 0x19F5;
const unsigned short PRODUCT_ID = 0xFE50;
const int INTERFACE_ID = 1;
const int REPORT_SIZE = 64;
const std::string PACKET_DISABLE_KB = "55a8";
const std::string PACKET_ENABLE_KB  = "55a9";
void debugLog(const std::string& msg);

static bool g_AlwaysOnTop = false;
static bool g_DisableKeyboardReporting = false;
static constexpr float kCustomTitleBarHeight = 30.0f;
static constexpr float kInitialMainPanelX = 10.0f;
static constexpr float kInitialMainPanelY = kCustomTitleBarHeight + 10.0f;
static constexpr float kInitialMainPanelW = 480.0f;
static constexpr float kInitialMainPanelH = 750.0f;
static constexpr float kInitialSettingsPanelX = 500.0f;
static constexpr float kInitialSettingsPanelY = kCustomTitleBarHeight + 10.0f;
static constexpr float kInitialSettingsPanelW = 260.0f;
static constexpr float kInitialSettingsPanelH = 220.0f;
static constexpr float kOuterPadding = 10.0f;
static constexpr BYTE kTransparentKeyR = 26;
static constexpr BYTE kTransparentKeyG = 26;
static constexpr BYTE kTransparentKeyB = 26;

std::wstring resolveIconPath(const std::wstring& fileName) {
    namespace fs = std::filesystem;

    std::vector<fs::path> candidates;
    candidates.emplace_back(fs::current_path() / fileName);

    wchar_t exePathRaw[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exePathRaw, MAX_PATH);
    if (len > 0) {
        fs::path exeDir = fs::path(exePathRaw).parent_path();
        candidates.emplace_back(exeDir / fileName);
        candidates.emplace_back(exeDir.parent_path() / fileName);
        candidates.emplace_back(exeDir.parent_path().parent_path() / fileName);
    }

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            return candidate.wstring();
        }
    }

    return (fs::current_path() / fileName).wstring();
}

void sendCommandPacket(hid_device* handle, const std::string& hex_str) {
    if (!handle) return;

    unsigned char buffer[65];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0x00; // Report ID

    for (size_t i = 0; i < hex_str.length() && i < 128; i += 2) {
        buffer[(i / 2) + 1] = static_cast<unsigned char>(std::stoul(hex_str.substr(i, 2), nullptr, 16));
    }

    hid_write(handle, buffer, 65);
}

void applyKeyboardReportingState(hid_device* handle, bool disableKeyboardReporting) {
    if (!handle) return;
    sendCommandPacket(handle, disableKeyboardReporting ? PACKET_DISABLE_KB : PACKET_ENABLE_KB);
    debugLog(disableKeyboardReporting
                 ? "[HID] Keyboard reporting disabled."
                 : "[HID] Keyboard reporting enabled.");
}

HICON loadPngAsHicon(const std::wstring& iconPath) {
    Gdiplus::Bitmap bitmap(iconPath.c_str());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }

    HICON icon = nullptr;
    if (bitmap.GetHICON(&icon) != Gdiplus::Ok) {
        return nullptr;
    }
    return icon;
}

struct PanelBounds {
    bool hasValue = false;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

// Calibration data: {key_id: {min_val, max_val}}
std::map<unsigned char, std::pair<int, int>> calibration;

// --- Magic Packets for keyboard unlock (same as Python, adjusted for C++) ---
// These are raw hex strings. Remember to prepend 0x00 if hid_write requires a Report ID.
std::vector<std::string> UNLOCK_PACKETS_HEX = {
    "55010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    "55050020200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    "5506002d200000005000aabb0400002a0764000000009cc6f800000000000000016400000100ff00000000000000000000000000000000000000000000000000"
};

// --- Function to combine Index 4 and 5 ---
int getAnalogValue(const unsigned char* report) {
    return (static_cast<int>(report[4]) << 8) | static_cast<int>(report[5]);
}

void unlockKeyboard(hid_device* dev_handle) {
    std::cout << "Attempting to wake up V-Muse65..." << std::endl;
    for (const auto& hex_str : UNLOCK_PACKETS_HEX) {
        std::vector<unsigned char> packet_bytes;
        // Convert hex string to bytes
        for (size_t i = 0; i < hex_str.length(); i += 2) {
            packet_bytes.push_back(static_cast<unsigned char>(std::stoul(hex_str.substr(i, 2), nullptr, 16)));
        }

        // HIDAPI on Windows: First byte MUST be the Report ID.
        // If the device doesn't use them, 0x00 is the marker.
        unsigned char buffer[65];
        memset(buffer, 0, sizeof(buffer));
        buffer[0] = 0x00;

        // Copy the hex data into the buffer starting at index 1
        size_t copy_len = (packet_bytes.size() > 64) ? 64 : packet_bytes.size();
        memcpy(&buffer[1], packet_bytes.data(), copy_len);

        // Send exactly 65 bytes (Report ID + 64 bytes of data)
        int res = hid_write(dev_handle, buffer, 65);

        if (res < 0) {
            // Use wprintf because hid_error returns a wide string (wchar_t*) on Windows
            fwprintf(stderr, L"HID Error: %s\n", hid_error(dev_handle));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// A list of all X360 buttons we want to map
std::vector<Binding> myBindings = {
    {"LS Up", 26}, {"LS Down", 22}, {"LS Left", 4}, {"LS Right", 7}, // Analog Stick
    {"Button A", 0}, {"Button B", 0}, {"Button X", 0}, {"Button Y", 0}, // Face Buttons
    {"LB", 0}, {"RB", 0},                                            // Shoulders
    {"LT", 0}, {"RT", 0},                                            // Triggers (Analog)
    {"Start", 0}, {"Back", 0},                                        // Menu
    {"D-Pad Up", 0}, {"D-Pad Down", 0}, {"D-Pad Left", 0}, {"D-Pad Right", 0}
};

const char* INPUT_CSV_PATH = "Input List.csv";
std::string g_InputCsvResolvedPath;

void debugLog(const std::string& msg) {
    std::cout << msg << std::endl;
    OutputDebugStringA((msg + "\n").c_str());

    std::ofstream log("VarMeControl_debug.log", std::ios::app);
    if (log.is_open()) {
        log << msg << "\n";
    }
}

std::string trimCopy(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string resolveCsvPath(const std::string& fileName) {
    namespace fs = std::filesystem;

    std::vector<fs::path> candidates;
    candidates.emplace_back(fs::current_path() / fileName);

    char exePathRaw[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, exePathRaw, MAX_PATH);
    if (len > 0) {
        fs::path exeDir = fs::path(exePathRaw).parent_path();
        candidates.emplace_back(exeDir / fileName);
        candidates.emplace_back(exeDir.parent_path() / fileName);
        candidates.emplace_back(exeDir.parent_path().parent_path() / fileName);
    }

    for (const auto& candidate : candidates) {
        debugLog("[CSV] Checking path: " + candidate.string());
        if (fs::exists(candidate)) {
            debugLog("[CSV] Using path: " + candidate.string());
            return candidate.string();
        }
    }

    debugLog("[CSV] No existing CSV found; defaulting to current directory path.");
    return (fs::current_path() / fileName).string();
}

std::vector<std::string> splitCsvSimple(const std::string& line) {
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) {
        cols.push_back(cell);
    }
    return cols;
}

bool loadInputCsv(const std::string& path,
                  std::vector<CsvInputRow>& rows,
                  std::map<unsigned char, std::string>& keyLabelById) {
    debugLog("[CSV] loadInputCsv() path=" + path);
    std::ifstream in(path);
    if (!in.is_open()) {
        debugLog("[CSV] Failed to open CSV for reading.");
        return false;
    }

    rows.clear();
    keyLabelById.clear();

    std::string line;
    if (!std::getline(in, line)) {
        debugLog("[CSV] Missing CSV header.");
        return false; // header
    }

    int parsedRows = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cols = splitCsvSimple(line);
        if (cols.size() < 2) continue;

        CsvInputRow row;
        row.keyLabel = trimCopy(cols[0]);
        try {
            row.number = std::stoi(trimCopy(cols[1]));
        } catch (...) {
            debugLog("[CSV] Invalid key number in row: " + line);
            continue;
        }
        row.control = (cols.size() >= 3) ? trimCopy(cols[2]) : "";

        rows.push_back(row);
        parsedRows++;
        if (!row.keyLabel.empty() && row.number >= 0 && row.number <= 255) {
            keyLabelById[static_cast<unsigned char>(row.number)] = row.keyLabel;
        }
    }

    debugLog("[CSV] Loaded rows=" + std::to_string(parsedRows) +
             " keyLabels=" + std::to_string(keyLabelById.size()));
    return true;
}

void initializeBindingsFromRows(const std::vector<CsvInputRow>& rows, std::vector<Binding>& bindings) {
    std::map<std::string, unsigned char> controlToKeyId;
    for (const auto& row : rows) {
        if (!row.control.empty() && row.number >= 0 && row.number <= 255) {
            controlToKeyId[row.control] = static_cast<unsigned char>(row.number);
        }
    }

    int initialized = 0;
    for (auto& bind : bindings) {
        auto it = controlToKeyId.find(bind.name);
        if (it != controlToKeyId.end()) {
            bind.keyID = it->second;
            initialized++;
            debugLog("[CSV] Init binding: " + bind.name + " <- keyID " + std::to_string(bind.keyID));
        }
    }
    debugLog("[CSV] Total initialized bindings: " + std::to_string(initialized));
}

bool saveInputCsv(const std::string& path,
                  const std::vector<Binding>& bindings,
                  std::vector<CsvInputRow>& rows) {
    debugLog("[CSV] saveInputCsv() path=" + path);
    if (rows.empty()) {
        debugLog("[CSV] Save aborted: rows empty.");
        return false;
    }

    std::map<unsigned char, std::string> keyIdToControl;
    for (const auto& bind : bindings) {
        if (bind.keyID != 0) {
            keyIdToControl[bind.keyID] = bind.name;
        }
    }

    for (auto& row : rows) {
        auto it = keyIdToControl.find(static_cast<unsigned char>(row.number));
        row.control = (it != keyIdToControl.end()) ? it->second : "";
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        debugLog("[CSV] Failed to open CSV for writing.");
        return false;
    }

    out << "Key,Number,Control\n";
    for (const auto& row : rows) {
        out << row.keyLabel << "," << row.number << "," << row.control << "\n";
    }

    debugLog("[CSV] Save complete. Wrote rows=" + std::to_string(rows.size()));
    return true;
}

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
void IncludePanelBounds(PanelBounds& bounds, const ImVec2& pos, const ImVec2& size);
void AutoFitWindowToPanels(HWND hwnd, const PanelBounds& bounds);

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

void IncludePanelBounds(PanelBounds& bounds, const ImVec2& pos, const ImVec2& size)
{
    if (size.x <= 0.0f || size.y <= 0.0f)
        return;

    const float right = pos.x + size.x;
    const float bottom = pos.y + size.y;
    if (!bounds.hasValue) {
        bounds.hasValue = true;
        bounds.maxX = right;
        bounds.maxY = bottom;
        return;
    }

    if (right > bounds.maxX) bounds.maxX = right;
    if (bottom > bounds.maxY) bounds.maxY = bottom;
}

void AutoFitWindowToPanels(HWND hwnd, const PanelBounds& bounds)
{
    if (!hwnd || !bounds.hasValue || IsZoomed(hwnd) || IsIconic(hwnd))
        return;

    const int desiredClientWidth = static_cast<int>(bounds.maxX + kOuterPadding);
    const int desiredClientHeight = static_cast<int>(((bounds.maxY > kCustomTitleBarHeight) ? bounds.maxY : kCustomTitleBarHeight) + kOuterPadding);

    RECT currentClient{};
    GetClientRect(hwnd, &currentClient);
    const int currentClientWidth = currentClient.right - currentClient.left;
    const int currentClientHeight = currentClient.bottom - currentClient.top;
    if (currentClientWidth == desiredClientWidth && currentClientHeight == desiredClientHeight)
        return;

    RECT rc = { 0, 0, desiredClientWidth, desiredClientHeight };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        rc.right - rc.left,
        rc.bottom - rc.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
    );
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
    case WM_NCCALCSIZE:
        // Remove native non-client area (default title bar/caption).
        if (wParam == TRUE)
            return 0;
        break;
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

hid_device* dev_handle = nullptr;
PVIGEM_CLIENT client = nullptr;
PVIGEM_TARGET pad = nullptr;

// --- YOUR MAIN ---
int main(int, char**) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    const bool gdiplusReady = (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok);
    HICON appIcon = nullptr;
    if (gdiplusReady) {
        appIcon = loadPngAsHicon(resolveIconPath(L"icon.png"));
    }

    std::vector<CsvInputRow> csvRows;
    std::map<unsigned char, std::string> keyLabelById;
    g_InputCsvResolvedPath = resolveCsvPath(INPUT_CSV_PATH);
    bool loadedCsv = loadInputCsv(g_InputCsvResolvedPath, csvRows, keyLabelById);
    if (!loadedCsv) {
        debugLog("[CSV] Startup load failed; continuing with default in-memory bindings.");
    }
    initializeBindingsFromRows(csvRows, myBindings);

    // Create application window
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0L;
    wc.cbWndExtra = 0L;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = appIcon;
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = L"VarMeControl";
    wc.hIconSm = appIcon;
    ::RegisterClassExW(&wc);
    const int desiredClientWidth = static_cast<int>((kInitialSettingsPanelX + kInitialSettingsPanelW) + kOuterPadding);
    const int desiredClientHeight = static_cast<int>((kInitialMainPanelY + kInitialMainPanelH) + kOuterPadding);
    const DWORD windowStyle = WS_OVERLAPPEDWINDOW & ~WS_CAPTION;
    RECT windowRect = { 0, 0, desiredClientWidth, desiredClientHeight };
    ::AdjustWindowRect(&windowRect, windowStyle, FALSE);

    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"V-Muse Analog Pro",
        windowStyle,
        100,
        100,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );
    if (appIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (appIcon) DestroyIcon(appIcon);
        if (gdiplusReady) Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    // Show the window
    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, RGB(kTransparentKeyR, kTransparentKeyG, kTransparentKeyB), 0, LWA_COLORKEY);

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
    // 1. Initialize HIDAPI
    if (hid_init() != 0) return -1;
    struct hid_device_info *devs, *cur_dev;
    devs = hid_enumerate(VENDOR_ID, PRODUCT_ID);
    cur_dev = devs;
    std::string path_to_open = "";

    while (cur_dev) {
        // Usage Page 0xFF00 is the industry standard for this.
        if (cur_dev->usage_page == 0xff00 || cur_dev->interface_number == 1) {
            path_to_open = cur_dev->path;
            break;
        }
        cur_dev = cur_dev->next;
    }

    if (!path_to_open.empty()) {
        dev_handle = hid_open_path(path_to_open.c_str());
    } else {
        // Fallback if we can't find the specific usage page
        //TODO: add dynamic detection for different keyboards
        dev_handle = hid_open(VENDOR_ID, PRODUCT_ID, NULL);
    }
    hid_free_enumeration(devs);

    if (!dev_handle) {
        std::cerr << "Could not find the correct V-Muse Raw HID interface!" << std::endl;
        return -1;
    }
    hid_set_nonblocking(dev_handle, 1);
    unlockKeyboard(dev_handle); // Wake up the analog sensors
    bool previousDisableKeyboardReporting = false;
    applyKeyboardReportingState(dev_handle, previousDisableKeyboardReporting);

    // 2. Initialize ViGEm
    client = vigem_alloc();
    if (client == nullptr) return -1;

    const auto verror = vigem_connect(client);
    if (!VIGEM_SUCCESS(verror)) return -1;

    pad = vigem_target_x360_alloc();
    vigem_target_add(client, pad);

    bool done = false;
    while (!done) {
        bool saveRequested = false;

        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Update window Z-order based on the checkbox
        HWND currentHwnd = GetActiveWindow(); // Or pass your 'hwnd' variable
        if (g_AlwaysOnTop) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        } else {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }

        // --- 1. HID READ LOGIC ---
        unsigned char report[64];
        int res = hid_read(dev_handle, report, sizeof(report));

        XUSB_REPORT xinput_report;
        XUSB_REPORT_INIT(&xinput_report);

        // 1. Analog Sticks (LS)
        float axisY = 0.0f;
        float axisX = 0.0f;
        axisY += myBindings[0].currentPercent; // Up
        axisY -= myBindings[1].currentPercent; // Down
        axisX -= myBindings[2].currentPercent; // Left
        axisX += myBindings[3].currentPercent; // Right
        xinput_report.sThumbLX = static_cast<SHORT>(axisX * 32767);
        xinput_report.sThumbLY = static_cast<SHORT>(axisY * 32767);

        // 2. Face Buttons (Bitmask)
        if (myBindings[4].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_A;
        if (myBindings[5].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_B;
        if (myBindings[6].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_X;
        if (myBindings[7].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_Y;

        // 3. Shoulders
        if (myBindings[8].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
        if (myBindings[9].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;

        // 4. Triggers (Analog 0-255)
        xinput_report.bLeftTrigger  = static_cast<BYTE>(myBindings[10].currentPercent * 255);
        xinput_report.bRightTrigger = static_cast<BYTE>(myBindings[11].currentPercent * 255);

        // 5. Menu Buttons
        if (myBindings[12].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_START;
        if (myBindings[13].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_BACK;

        // 6. D-Pad
        if (myBindings[14].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_DPAD_UP;
        if (myBindings[15].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
        if (myBindings[16].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
        if (myBindings[17].currentPercent > 0.1f) xinput_report.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;

        // Send to virtual controller
        if (client && pad) {
            vigem_target_x360_update(client, pad, xinput_report);
        }

        if (res > 0) {
            unsigned char incomingKeyID = report[3];
            int rawVal = (static_cast<int>(report[4]) << 8) | static_cast<int>(report[5]);

            for (auto& bind : myBindings) {
                // --- REBINDING ---
                if (bind.isWaiting && incomingKeyID != 0) {
                    bind.keyID = incomingKeyID;
                    bind.minRaw = rawVal; // Set baseline to current pressure
                    bind.maxRaw = rawVal; // Temporary ceiling
                    bind.isWaiting = false;
                    saveRequested = true;
                }

                // --- UPDATING VALUE ---
                if (bind.keyID == incomingKeyID && incomingKeyID != 0) {
                    // Auto-calibrate Max
                    if (rawVal > bind.maxRaw) bind.maxRaw = rawVal;
                    // Auto-calibrate Min (Deadzone)
                    if (rawVal < bind.minRaw) bind.minRaw = rawVal;

                    // Calculate percentage based on the range we've actually seen
                    float range = static_cast<float>(bind.maxRaw - bind.minRaw);
                    if (range > 0) {
                        float val = static_cast<float>(rawVal - bind.minRaw) / range;
                        bind.currentPercent = (val < 0.0f) ? 0.0f : (val > 1.0f ? 1.0f : val);
                    }
                }
            }
        }
        // REMOVE the "else { bind.currentPercent *= 0.9f; }" block entirely.
        // The keys should stay at their last known value until the keyboard sends a '0' pressure report.

        // --- 2. START IMGUI FRAME ---
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // --- CUSTOM TITLE BAR ---
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, kCustomTitleBarHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.14f, 0.14f, 0.14f, 1.0f));
        ImGui::Begin(
            "##CustomTitleBar",
            nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse
        );

        const float buttonSize = kCustomTitleBarHeight - 8.0f;
        const float buttonSpacing = 4.0f;
        const float buttonGroupWidth = (buttonSize * 3.0f) + (buttonSpacing * 2.0f) + 8.0f;
        const float dragWidth = ImGui::GetWindowWidth() - buttonGroupWidth;

        ImGui::InvisibleButton("##TitleDragZone", ImVec2(dragWidth, kCustomTitleBarHeight));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 titlePos = ImGui::GetWindowPos();
        drawList->AddText(
            ImVec2(titlePos.x + 10.0f, titlePos.y + 7.0f),
            IM_COL32(230, 230, 230, 255),
            "V-Muse Analog Pro"
        );

        ImGui::SetCursorPos(ImVec2(dragWidth + 4.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.34f, 0.34f, 0.34f, 1.0f));

        if (ImGui::Button("_", ImVec2(buttonSize, buttonSize))) {
            ShowWindow(hwnd, SW_MINIMIZE);
        }
        ImGui::SameLine(0.0f, buttonSpacing);
        if (ImGui::Button(IsZoomed(hwnd) ? "R" : "M", ImVec2(buttonSize, buttonSize))) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        }
        ImGui::SameLine(0.0f, buttonSpacing);
        if (ImGui::Button("X", ImVec2(buttonSize, buttonSize))) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }

        ImGui::PopStyleColor(3);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        // --- 3. THE GUI WINDOW ---
        // Set initial panel size/position - only applies on first run (or when .ini is reset)
        PanelBounds panelBounds;
        ImGui::SetNextWindowPos(ImVec2(kInitialMainPanelX, kInitialMainPanelY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(kInitialMainPanelW, kInitialMainPanelH), ImGuiCond_FirstUseEver);

        ImGui::Begin("Controller Remapper", nullptr, ImGuiWindowFlags_NoCollapse);
        IncludePanelBounds(panelBounds, ImGui::GetWindowPos(), ImGui::GetWindowSize());

        // Bindings list + save button
        ImGui::BeginChild("BindingsPanel", ImVec2(0, 0), true);

        for (auto& bind : myBindings) {
            ImGui::PushID(bind.name.c_str());

            // Trash/Clear Button
            if (ImGui::Button("X")) {
                bind.keyID = 0;
                bind.isWaiting = false;
            }

            ImGui::SameLine();
            ImGui::Text("%-18s", bind.name.c_str());

            ImGui::SameLine();
            std::string btnText = bind.isWaiting ? "???" : (bind.keyID == 0 ? "None" : std::to_string(bind.keyID));
            if (!bind.isWaiting && bind.keyID != 0) {
                auto keyIt = keyLabelById.find(bind.keyID);
                if (keyIt != keyLabelById.end() && !keyIt->second.empty()) {
                    btnText = keyIt->second;
                }
            }
            if (ImGui::Button(btnText.c_str(), ImVec2(80, 0))) {
                for (auto& b : myBindings) b.isWaiting = false;
                bind.isWaiting = true;
            }

            ImGui::SameLine();
            ImGui::ProgressBar(bind.currentPercent, ImVec2(-FLT_MIN, 0));

            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Button("Save Configuration", ImVec2(-FLT_MIN, 40))) {
            saveRequested = true;
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(kInitialSettingsPanelX, kInitialSettingsPanelY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(kInitialSettingsPanelW, kInitialSettingsPanelH), ImGuiCond_FirstUseEver);
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoCollapse);
        IncludePanelBounds(panelBounds, ImGui::GetWindowPos(), ImGui::GetWindowSize());
        ImGui::Checkbox("Pin to Top", &g_AlwaysOnTop);
        ImGui::Checkbox("Disable Keyboard Reporting", &g_DisableKeyboardReporting);
        if (g_DisableKeyboardReporting != previousDisableKeyboardReporting) {
            applyKeyboardReportingState(dev_handle, g_DisableKeyboardReporting);
            previousDisableKeyboardReporting = g_DisableKeyboardReporting;
        }
        ImGui::Separator();

        if (ImGui::Button("Clear All", ImVec2(-FLT_MIN, 0))) {
            for (auto& b : myBindings) {
                b.keyID = 0;
                b.isWaiting = false;
                b.currentPercent = 0.0f;
            }
            saveRequested = true;
        }

        if (ImGui::Button("Reset Calibration", ImVec2(-FLT_MIN, 0))) {
            for (auto& b : myBindings) {
                b.minRaw = 100;
                b.maxRaw = 800;
            }
        }
        ImGui::End();

        AutoFitWindowToPanels(hwnd, panelBounds);

        if (saveRequested) {
            saveInputCsv(g_InputCsvResolvedPath, myBindings, csvRows);
        }

        // --- 4. RENDERING ---
        ImGui::Render();
        const float clear_color_with_alpha[4] = {
            kTransparentKeyR / 255.0f,
            kTransparentKeyG / 255.0f,
            kTransparentKeyB / 255.0f,
            1.0f
        };
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

    // Final Cleanup
    // RE-ENABLE standard Windows key reporting before exiting
    if (dev_handle) {
        applyKeyboardReportingState(dev_handle, false);
    }
    if (pad) {
        vigem_target_remove(client, pad);
        vigem_target_free(pad);
    }
    if (client) {
        vigem_disconnect(client);
        vigem_free(client);
    }
    if (dev_handle) {
        hid_close(dev_handle);
    }
    hid_exit();
    if (appIcon) {
        DestroyIcon(appIcon);
    }
    if (gdiplusReady) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    return 0;
}

// --- BOILERPLATE WIN32/D3D FUNCTIONS ---
// (Paste these exactly as is from the ImGui 'main.cpp' example for Win32+DX11)
