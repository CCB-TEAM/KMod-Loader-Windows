// In-game ImGui overlay: see overlay.h. Direct3D 11 and 12 are both supported; which one the game uses is found out from its swap chain.
#include "overlay.h"

#ifdef KMOD_OVERLAY

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include "cmdq.h"
#include "kmod_common.h"
#include "kmod_log.h"
#include "kmod_version.h"
#include "x64.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
enum class Api { None, DX11, DX12 };

std::atomic<bool> g_visible{false};
std::mutex g_snapMu;
std::string g_snap;
std::string g_status = "not started";

Api g_api = Api::None;
bool g_ready = false, g_failed = false;
HWND g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;
double g_startTime = 0;
long long g_frames = 0;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn g_origPresent = nullptr;
Present1Fn g_origPresent1 = nullptr;
thread_local bool t_inPresent = false;

double Now()
{
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------------------------- DX12 state
struct Dx12
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    UINT rtvInc = 0;
    UINT bufCount = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12CommandAllocator* alloc[8] = {};
    UINT64 frameFence[8] = {};
    ID3D12Resource* rtvFor[8] = {};         // which buffer each RTV descriptor was made for
} g12;

struct Dx11
{
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
} g11;

// ---------------------------------------------------------------------------------------------------------------- UI
struct ModRow { std::string id, title, version, status; bool enabled; };

std::vector<ModRow> ParseSnapshot()
{
    std::string text;
    { std::lock_guard<std::mutex> g(g_snapMu); text = g_snap; }
    std::vector<ModRow> rows;
    size_t i = 0;
    while (i <= text.size() && !text.empty())
    {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        std::vector<std::string> f;
        size_t p = 0;
        for (;;)
        {
            size_t t = line.find('\t', p);
            f.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
        }
        if (f.size() >= 5) rows.push_back({f[0], f[1], f[2], f[4], f[3] == "1"});
        i = e + 1;
    }
    return rows;
}

void DrawMods()
{
    if (ImGui::Button("Reload mods")) CmdQueue_Push("reload");
    ImGui::SameLine();
    ImGui::TextDisabled("(the list refreshes about once a second)");
    auto rows = ParseSnapshot();
    if (rows.empty()) { ImGui::TextUnformatted("No mods found. Put mod folders or .kmod packages into kmod/mods."); return; }
    if (ImGui::BeginTable("mods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (auto& r : rows)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = r.enabled;
            if (ImGui::Checkbox(("##" + r.id).c_str(), &on)) CmdQueue_Push((on ? "enable " : "disable ") + r.id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.title.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.id.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.version.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r.status.c_str());
        }
        ImGui::EndTable();
    }
}

void DrawConsole()
{
    static bool showDebug = false, stick = true;
    static char input[1024] = "";
    ImGui::Checkbox("debug lines", &showDebug);
    ImGui::SameLine();
    ImGui::Checkbox("follow", &stick);
    ImGui::SameLine();
    if (ImGui::Button("help")) CmdQueue_Push("help");
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("log", ImVec2(0, avail.y - ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    auto lines = LogRing_Snapshot();
    size_t from = lines.size() > 600 ? lines.size() - 600 : 0;
    for (size_t i = from; i < lines.size(); i++)
    {
        const LogLine& l = lines[i];
        if (l.level == 0 && !showDebug) continue;
        ImVec4 c = l.level >= 3 ? ImVec4(1.0f, 0.4f, 0.4f, 1) : l.level == 2 ? ImVec4(1.0f, 0.85f, 0.3f, 1) : l.level == 0 ? ImVec4(0.6f, 0.6f, 0.6f, 1) : ImVec4(0.9f, 0.9f, 0.9f, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
    }
    if (stick && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-60);
    bool send = ImGui::InputText("##input", input, sizeof(input), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Run")) send = true;
    if (send && input[0])
    {
        CmdQueue_Push(input);
        input[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void DrawAbout()
{
    ImGui::Text("kmod loader %s (%s, Windows)", KMOD_VERSION, KMOD_FLAVOR);
    ImGui::Text("renderer: %s", g_api == Api::DX12 ? "Direct3D 12" : g_api == Api::DX11 ? "Direct3D 11" : "?");
    ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / std::max(1.0f, ImGui::GetIO().Framerate));
    ImGui::Separator();
    ImGui::TextUnformatted("Insert: show / hide this menu");
    ImGui::TextUnformatted("Console: the kmod log, and console commands or Lua expressions (help lists the commands).");
}

void BuildUi()
{
    ImGuiIO& io = ImGui::GetIO();
    if (!g_visible)
    {
        // a short hint after start-up, then nothing is drawn at all
        ImGui::SetNextWindowPos(ImVec2(12, 12));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("kmod %s - press Insert for the menu", KMOD_VERSION);
        ImGui::End();
        return;
    }
    io.MouseDrawCursor = true;
    ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 60), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("kmod loader", &open))
    {
        if (ImGui::BeginTabBar("tabs"))
        {
            if (ImGui::BeginTabItem("Mods")) { DrawMods(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Console")) { DrawConsole(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("About")) { DrawAbout(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) g_visible = false;
    if (!g_visible) io.MouseDrawCursor = false;
}

// ---------------------------------------------------------------------------------------------------------------- input
LRESULT CALLBACK HkWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_INSERT && !(l & (1 << 30))) { g_visible = !g_visible; ImGui::GetIO().MouseDrawCursor = g_visible; }
    if (g_visible && g_ready)
    {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        ImGuiIO& io = ImGui::GetIO();
        const bool mouse = (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_NCMOUSEMOVE || m == WM_INPUT;
        const bool key = m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR;
        if (io.WantCaptureMouse && mouse && m != WM_INPUT) return 0;
        if (io.WantCaptureKeyboard && key) return 0;
        if (m == WM_SETCURSOR && io.WantCaptureMouse) return TRUE;
    }
    return CallWindowProcW(g_origProc, h, m, w, l);
}

// ---------------------------------------------------------------------------------------------------------------- init
void SetupImGui()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    // Chinese text: the system's Microsoft YaHei (falls back to ImGui's built-in font when it is missing)
    const char* fonts[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\simsun.ttc"};
    for (const char* f : fonts)
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(f, 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) break;
    ImGui_ImplWin32_Init(g_hwnd);
    g_origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HkWndProc)));
}

bool InitDx12(IDXGISwapChain* sc, ID3D12CommandQueue* queue, const DXGI_SWAP_CHAIN_DESC& d)
{
    Dx12& g = g12;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&g.device)))) { g_status = "DX12: no device"; return false; }
    g.queue = queue;
    g.bufCount = std::min<UINT>(std::max<UINT>(d.BufferCount, 2), 8);
    g.format = d.BufferDesc.Format;
    D3D12_DESCRIPTOR_HEAP_DESC rd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, g.bufCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC sd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g.device->CreateDescriptorHeap(&rd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g.rtvHeap))) ||
        FAILED(g.device->CreateDescriptorHeap(&sd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g.srvHeap)))) { g_status = "DX12: descriptor heaps"; return false; }
    g.rtvInc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < g.bufCount; i++)
        if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&g.alloc[i])))) { g_status = "DX12: allocator"; return false; }
    if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g.list)))) { g_status = "DX12: command list"; return false; }
    g.list->Close();
    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g.fence)))) { g_status = "DX12: fence"; return false; }
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    SetupImGui();
    ImGui_ImplDX12_InitInfo info;
    info.Device = g.device;
    info.CommandQueue = g.queue;
    info.NumFramesInFlight = (int)g.bufCount;
    info.RTVFormat = g.format;
    info.SrvDescriptorHeap = g.srvHeap;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
    { *cpu = i->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(); *gpu = i->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart(); };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
    if (!ImGui_ImplDX12_Init(&info)) { g_status = "DX12: ImGui backend"; return false; }
    return true;
}

bool InitDx11(ID3D11Device* dev)
{
    g11.device = dev;
    dev->GetImmediateContext(&g11.ctx);
    SetupImGui();
    if (!ImGui_ImplDX11_Init(g11.device, g11.ctx)) { g_status = "DX11: ImGui backend"; return false; }
    return true;
}

bool TryInit(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) { g_status = "swap chain without a window"; return false; }
    g_hwnd = d.OutputWindow;
    ID3D12CommandQueue* queue = nullptr;
    ID3D11Device* dev11 = nullptr;
    HRESULT h12 = sc->GetDevice(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (SUCCEEDED(h12) && queue)
    {
        g_api = Api::DX12;
        KLog(KLOG_INFO, "overlay", "the game renders with Direct3D 12 (window %p, %ux%u, format %d, %u buffers)", g_hwnd, d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount);
        return InitDx12(sc, queue, d);
    }
    HRESULT h11 = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev11));
    if (SUCCEEDED(h11) && dev11)
    {
        g_api = Api::DX11;
        KLog(KLOG_INFO, "overlay", "the game renders with Direct3D 11 (window %p, %ux%u, format %d)", g_hwnd, d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format);
        return InitDx11(dev11);
    }
    char b[160]; snprintf(b, sizeof(b), "swap chain device query failed (queue 0x%08lx, device 0x%08lx)", (unsigned long)h12, (unsigned long)h11);
    g_status = b;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------- frame
void RenderDx11(IDXGISwapChain* sc)
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return;
    ID3D11RenderTargetView* rtv = nullptr;
    g11.device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (!rtv) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();
    g11.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    rtv->Release();
}

void RenderDx12(IDXGISwapChain* sc)
{
    Dx12& g = g12;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) || !sc3) return;
    UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    if (idx >= g.bufCount) return;
    ID3D12Resource* back = nullptr;
    if (FAILED(sc->GetBuffer(idx, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&back))) || !back) return;
    back->Release();                                           // the swap chain keeps it alive; only the pointer identity matters
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * g.rtvInc;
    if (g.rtvFor[idx] != back) { g.device->CreateRenderTargetView(back, nullptr, rtv); g.rtvFor[idx] = back; }

    // this frame's allocator may still be in use by the GPU
    if (g.frameFence[idx] && g.fence->GetCompletedValue() < g.frameFence[idx])
    {
        g.fence->SetEventOnCompletion(g.frameFence[idx], g.fenceEvent);
        WaitForSingleObject(g.fenceEvent, 1000);
    }
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();

    g.alloc[idx]->Reset();
    g.list->Reset(g.alloc[idx], nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->ResourceBarrier(1, &b);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g.srvHeap};
    g.list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g.list);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->ResourceBarrier(1, &b);
    g.list->Close();
    ID3D12CommandList* lists[] = {g.list};
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.fenceValue);
    g.frameFence[idx] = g.fenceValue;
}

void OnFrame(IDXGISwapChain* sc)
{
    if (g_failed) return;
    if (!g_ready)
    {
        if (!TryInit(sc))
        {
            static int tries = 0;
            if (++tries == 1 || tries % 300 == 0) KLog(KLOG_WARN, "overlay", "initialisation failed (attempt %d): %s", tries, g_status.c_str());
            if (tries >= 1200) { g_failed = true; KLog(KLOG_ERROR, "overlay", "giving up: %s", g_status.c_str()); }
            return;
        }
        g_ready = true;
        g_startTime = Now();
        g_status = std::string("running (") + (g_api == Api::DX12 ? "Direct3D 12" : "Direct3D 11") + ")";
        KLog(KLOG_INFO, "overlay", "ready: %s; press Insert to open the menu", g_status.c_str());
    }
    if (!g_visible && Now() - g_startTime > 12.0) return;           // nothing to draw: leave the frame alone
    if (g_api == Api::DX12) RenderDx12(sc); else RenderDx11(sc);
    if (++g_frames == 1) KLog(KLOG_INFO, "overlay", "first frame drawn");
}

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!t_inPresent)
    {
        t_inPresent = true;
        OnFrame(sc);
        HRESULT r = g_origPresent(sc, sync, flags);
        t_inPresent = false;
        return r;
    }
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    if (!t_inPresent)
    {
        t_inPresent = true;
        OnFrame(sc);
        HRESULT r = g_origPresent1(sc, sync, flags, p);
        t_inPresent = false;
        return r;
    }
    return g_origPresent1(sc, sync, flags, p);
}

// ---------------------------------------------------------------------------------------------------------------- hooking
// A throw-away window and swap chain give the address of dxgi's Present: the same code serves the game's own swap chain.
bool FindPresent(void*& present, void*& present1)
{
    using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
                                      IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    CreateFn create = d3d11 ? reinterpret_cast<CreateFn>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    if (!create) return false;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"kmod_overlay_probe";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return false;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    bool ok = SUCCEEDED(hr) && sc;
    if (ok)
    {
        void** vt = *reinterpret_cast<void***>(sc);
        present = vt[8];
        present1 = vt[22];
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

void StartMain()
{
    // wait for the graphics runtime: the game creates its swap chain after the engine is up
    for (int i = 0; i < 600 && !GetModuleHandleW(L"dxgi.dll"); i++) Sleep(100);
    void *present = nullptr, *present1 = nullptr;
    if (!FindPresent(present, present1)) { g_status = "could not find IDXGISwapChain::Present"; KLog(KLOG_ERROR, "overlay", "%s", g_status.c_str()); return; }
    KLog(KLOG_INFO, "overlay", "dxgi Present at %p, Present1 at %p", present, present1);
    void* orig = nullptr;
    if (!KMod_InstallHook(present, reinterpret_cast<void*>(&HkPresent), &orig)) { g_status = "Present hook failed"; return; }
    g_origPresent = reinterpret_cast<PresentFn>(orig);
    if (present1 && present1 != present)
    {
        void* orig1 = nullptr;
        if (KMod_InstallHook(present1, reinterpret_cast<void*>(&HkPresent1), &orig1)) g_origPresent1 = reinterpret_cast<Present1Fn>(orig1);
        else KLog(KLOG_WARN, "overlay", "Present1 hook failed (the game may use only Present)");
    }
    g_status = "hooked, waiting for the first frame";
    KLog(KLOG_INFO, "overlay", "Present hooked");
}
}

void Overlay_Start()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    if (GetFileAttributesW(KWide(KModRoot() + "/no_overlay").c_str()) != INVALID_FILE_ATTRIBUTES) { g_status = "disabled (kmod/no_overlay exists)"; KLog(KLOG_INFO, "overlay", "%s", g_status.c_str()); return; }
    std::thread(StartMain).detach();
}

bool Overlay_Visible() { return g_visible.load() && g_ready; }

void Overlay_SetSnapshot(const std::string& text)
{
    std::lock_guard<std::mutex> g(g_snapMu);
    g_snap = text;
}

std::string Overlay_Status() { return g_status; }

#endif   // KMOD_OVERLAY
