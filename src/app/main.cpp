#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static App g_app;
static float g_dpi = 1.0f;
static bool g_rebuildFonts = false;

static void createRT() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &sd, &g_swap,
                                             &g_dev, &got, &g_ctx)))
        return false;
    createRT();
    return true;
}

static void buildStyleAndFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x21FF, 0x00D7, 0x00D7, 0};
    if (!io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", std::round(16.0f * g_dpi), &cfg, ranges))
        io.Fonts->AddFontDefault();
    io.Fonts->Build();

    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0;
    style.ChildRounding = 6;
    style.FrameRounding = 4;
    style.PopupRounding = 6;
    style.GrabRounding = 4;
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.ChildBorderSize = 1;
    style.FrameBorderSize = 0;
    style.ScrollbarSize = 12;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.078f, 0.086f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.15f, 1);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.92f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.52f, 0.55f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.19f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.22f, 0.25f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.26f, 0.29f, 1);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.17f, 0.19f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.21f, 0.22f, 0.25f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.27f, 0.30f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.55f, 0.99f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.33f, 0.62f, 1.00f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.33f, 0.62f, 1.00f, 1);
    c[ImGuiCol_Header] = ImVec4(0.24f, 0.55f, 0.99f, 0.22f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.55f, 0.99f, 0.30f);
    style.ScaleAllSizes(g_dpi);
    ImGui::GetStyle() = style;
}

// --capture: render the app's own backbuffer to a PNG (hidden window, for tests).
static bool saveBackbuffer(const std::wstring& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Texture2D> back, staging;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    g_ctx->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::vector<uint8_t> bgra(size_t(td.Width) * td.Height * 4);
    for (UINT y = 0; y < td.Height; ++y) {
        const uint8_t* s = (const uint8_t*)m.pData + size_t(y) * m.RowPitch;
        uint8_t* d = bgra.data() + size_t(y) * td.Width * 4;
        for (UINT x = 0; x < td.Width; ++x) { d[x * 4] = s[x * 4 + 2]; d[x * 4 + 1] = s[x * 4 + 1]; d[x * 4 + 2] = s[x * 4]; d[x * 4 + 3] = 255; }
    }
    g_ctx->Unmap(staging.Get(), 0);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(td.Width, td.Height);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WritePixels(td.Height, td.Width * 4, UINT(bgra.size()), bgra.data());
    frame->Commit();
    return SUCCEEDED(enc->Commit());
}

static LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(drop, i, p.data(), len + 1);
            paths.push_back(p);
        }
        DragFinish(drop);
        g_app.addPaths(paths);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp) / 96.0f;
        g_rebuildFonts = true;
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = {LONG(900 * g_dpi), LONG(600 * g_dpi)};
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, wndProc, 0, 0, inst, LoadIconW(inst, L"IDI_APP"), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
                   L"GoodHAP", nullptr};
    RegisterClassExW(&wc);
    POINT pt{0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
        if (auto fn = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor")) fn(mon, 0, &dpiX, &dpiY);
    g_dpi = dpiX / 96.0f;

    // Arguments: files to queue, or --capture out.png [--size W H] [--view N] [--diff]
    // [--zoom Z CX CY] [--frame N] [--exposure E] files...
    std::vector<std::wstring> files;
    std::wstring capturePath;
    int capW = 1440, capH = 900, capView = -1;
    bool capDiff = false, capEncode = false;
    float capZoom = 0, capCx = 0, capCy = 0, capExposure = 0, capCompact = -1;
    long long capFrame = 0;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            std::wstring a = argv[i];
            auto num = [&] { return i + 1 < argc ? _wtof(argv[++i]) : 0.0; };
            if (a == L"--capture" && i + 1 < argc) capturePath = argv[++i];
            else if (a == L"--size") { capW = int(num()); capH = int(num()); }
            else if (a == L"--view") capView = int(num());
            else if (a == L"--diff") capDiff = true;
            else if (a == L"--encode") capEncode = true;
            else if (a == L"--zoom") { capZoom = float(num()); capCx = float(num()); capCy = float(num()); }
            else if (a == L"--frame") capFrame = (long long)num();
            else if (a == L"--exposure") capExposure = float(num());
            else if (a == L"--compact") capCompact = float(num());
            else files.push_back(a);
        }
        if (argv) LocalFree(argv);
    }
    const bool capture = !capturePath.empty();
    if (capture) g_dpi = 1.0f;

    RECT wr{0, 0, capture ? capW : int(1440 * g_dpi), capture ? capH : int(900 * g_dpi)};
    if (capture) AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"GoodHAP — Hap Q encoder", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    if (!capture) g_dpi = GetDpiForWindow(hwnd) / 96.0f;
    if (!createDevice(hwnd)) {
        MessageBoxW(hwnd, L"Could not create a Direct3D 11 device.", L"GoodHAP", MB_ICONERROR);
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    if (!capture) {
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // No keyboard nav: Space (flip to source) and the arrows (step frames) belong to the preview.
    buildStyleAndFonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    g_app.init(hwnd, g_dev, g_ctx, g_dpi);

    // Files passed on the command line (or dropped on the exe) start in the queue.
    if (!files.empty()) g_app.addPaths(files);
    if (capture) {
        g_app.setCaptureMode(capCompact);
        auto& v = g_app.viewParams();
        if (capView >= 0) v.view = PreviewRenderer::View(capView);
        v.difference = capDiff;
        v.exposure = capExposure;
        g_app.setPreviewFrame(capFrame);
        if (capZoom > 0) { g_app.setFit(false); v.zoom = capZoom; v.centerX = capCx; v.centerY = capCy; }
    }

    bool done = false;
    int idleFrames = 0, settledFrames = 0;
    ULONGLONG captureStart = GetTickCount64();
    while (!done) {
        // Sleep while nothing happens; keep drawing while work runs in the background.
        if (!capture && !g_app.busy() && idleFrames > 3) MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        bool hadInput = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
            hadInput = true;
        }
        idleFrames = hadInput ? 0 : idleFrames + 1;
        if (done) break;
        if (g_resizeW) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            createRT();
        }
        if (g_rebuildFonts) {
            g_rebuildFonts = false;
            buildStyleAndFonts();
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        g_app.frame();
        ImGui::Render();
        const float clear[4] = {0.075f, 0.078f, 0.086f, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (capture) {
            bool ready = g_app.previewSettled() && (!capEncode || g_app.autoEncodeStep());
            settledFrames = ready ? settledFrames + 1 : 0;
            bool timeout = GetTickCount64() - captureStart > (capEncode ? 1800000 : 120000);
            if (settledFrames > 5 || timeout || (files.empty() && GetTickCount64() - captureStart > 1000)) {
                saveBackbuffer(capturePath);
                done = true;
            }
            g_swap->Present(0, 0);
            continue;
        }
        g_swap->Present(1, 0);
    }

    g_app.shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    g_swap->Release();
    g_ctx->Release();
    g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    return 0;
}
