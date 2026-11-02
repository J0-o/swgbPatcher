#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <ddraw.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11Texture2D* g_backBuffer = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
ID3D11Texture2D* g_indexTexture = nullptr;
ID3D11ShaderResourceView* g_indexView = nullptr;
ID3D11Texture2D* g_paletteTexture = nullptr;
ID3D11ShaderResourceView* g_paletteView = nullptr;
ID3D11VertexShader* g_vertexShader = nullptr;
ID3D11PixelShader* g_pixelShader = nullptr;
HWND g_window = nullptr;
HWND g_subclassedWindow = nullptr;
WNDPROC g_originalWindowProc = nullptr;
constexpr UINT kCenterMenuWindowMessage = WM_APP + 0x3D1;
LONG g_menuWindowX = 0;
LONG g_menuWindowY = 0;
LONG g_menuWindowWidth = 0;
LONG g_menuWindowHeight = 0;
DWORD g_displayWidth = 800;
DWORD g_displayHeight = 600;
std::atomic<unsigned long> g_presentCount{0};
std::atomic<unsigned long> g_paletteGeneration{1};
char g_logPath[MAX_PATH]{};
class NativeDirectDraw;
class NativeSurface;
class NativePalette;
class NativeClipper;

HRESULT WINAPI GbDirectDrawCreate(GUID*, LPDIRECTDRAW*, IUnknown*);

void Log(const char* message) {
    OutputDebugStringA(message);
    if (!g_logPath[0]) return;
    HANDLE file = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    CloseHandle(file);
}

void LogResult(const char* method, HRESULT result) {
    char message[160]{};
    std::snprintf(message, sizeof(message), "[GB D3D11] %s -> 0x%08lX.\r\n",
                  method, static_cast<unsigned long>(result));
    Log(message);
}

void LogSampled(const char* method, std::atomic<unsigned long>& counter) {
    const unsigned long count = ++counter;
    if (count <= 16 || (count & (count - 1)) == 0) {
        char message[128]{};
        std::snprintf(message, sizeof(message), "[GB D3D11] %s call #%lu.\r\n", method, count);
        Log(message);
    }
}

void SetLogPath(HMODULE module) {
    const DWORD length = GetModuleFileNameA(module, g_logPath, MAX_PATH);
    if (!length || length >= MAX_PATH) { g_logPath[0] = '\0'; return; }
    char* name = g_logPath + length;
    while (name != g_logPath && name[-1] != '\\' && name[-1] != '/') --name;
    lstrcpyA(name, "gb-d3d11-bootstrap.log");
}

bool IsMenuMode() {
    return g_displayWidth == 800 && g_displayHeight == 600;
}

bool IsClientMouseMessage(UINT message) {
    switch (message) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK MenuWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kCenterMenuWindowMessage) {
        if (IsMenuMode() && window == g_window && g_menuWindowWidth > 0 && g_menuWindowHeight > 0) {
            SetWindowPos(window, nullptr, g_menuWindowX, g_menuWindowY, g_menuWindowWidth, g_menuWindowHeight,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_SHOWWINDOW);
            Log("[GB D3D11] Deferred menu centering applied after mode transition.\r\n");
        }
        return 0;
    }
    if (IsMenuMode() && IsClientMouseMessage(message)) {
        RECT client{};
        if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
            const LONG x = std::clamp<LONG>(static_cast<LONG>(MulDiv(GET_X_LPARAM(lParam), 800, client.right)), 0, 799);
            const LONG y = std::clamp<LONG>(static_cast<LONG>(MulDiv(GET_Y_LPARAM(lParam), 600, client.bottom)), 0, 599);
            lParam = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
        }
    }
    return CallWindowProcA(g_originalWindowProc, window, message, wParam, lParam);
}

void InstallMenuWindowSubclass(HWND window) {
    if (!window || window == g_subclassedWindow) return;
    if (g_subclassedWindow && g_originalWindowProc && IsWindow(g_subclassedWindow))
        SetWindowLongPtrA(g_subclassedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWindowProc));
    g_subclassedWindow = nullptr;
    g_originalWindowProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWindowProc)));
    if (g_originalWindowProc) g_subclassedWindow = window;
    else Log("[GB D3D11] Window subclass installation failed; resized menu mouse mapping is unavailable.\r\n");
}

void ReleaseSwapChain() {
    if (g_pixelShader) { g_pixelShader->Release(); g_pixelShader = nullptr; }
    if (g_vertexShader) { g_vertexShader->Release(); g_vertexShader = nullptr; }
    if (g_paletteView) { g_paletteView->Release(); g_paletteView = nullptr; }
    if (g_paletteTexture) { g_paletteTexture->Release(); g_paletteTexture = nullptr; }
    if (g_indexView) { g_indexView->Release(); g_indexView = nullptr; }
    if (g_indexTexture) { g_indexTexture->Release(); g_indexTexture = nullptr; }
    if (g_renderTarget) { g_renderTarget->Release(); g_renderTarget = nullptr; }
    if (g_backBuffer) { g_backBuffer->Release(); g_backBuffer = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
}

HRESULT CreatePalettePipeline() {
    static const char vertexSource[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {"
        "float2 positions[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };"
        "return float4(positions[id], 0, 1); }";
    static const char pixelSource[] =
        "Texture2D<uint> indices : register(t0);"
        "Texture2D<float4> palette : register(t1);"
        "float4 main(float4 position : SV_Position) : SV_Target {"
        "uint index = indices.Load(int3(int2(position.xy), 0));"
        "return palette.Load(int3(index, 0, 0)); }";

    ID3DBlob* vertexBytecode = nullptr;
    ID3DBlob* pixelBytecode = nullptr;
    HRESULT result = D3DCompile(vertexSource, sizeof(vertexSource) - 1, nullptr, nullptr, nullptr,
                                "main", "vs_4_0", 0, 0, &vertexBytecode, nullptr);
    if (SUCCEEDED(result)) result = D3DCompile(pixelSource, sizeof(pixelSource) - 1, nullptr, nullptr, nullptr,
                                               "main", "ps_4_0", 0, 0, &pixelBytecode, nullptr);
    if (SUCCEEDED(result)) result = g_device->CreateVertexShader(vertexBytecode->GetBufferPointer(),
                                                                 vertexBytecode->GetBufferSize(), nullptr, &g_vertexShader);
    if (SUCCEEDED(result)) result = g_device->CreatePixelShader(pixelBytecode->GetBufferPointer(),
                                                                pixelBytecode->GetBufferSize(), nullptr, &g_pixelShader);
    if (pixelBytecode) pixelBytecode->Release();
    if (vertexBytecode) vertexBytecode->Release();

    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = g_displayWidth;
    texture.Height = g_displayHeight;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8_UINT;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(result)) result = g_device->CreateTexture2D(&texture, nullptr, &g_indexTexture);
    if (SUCCEEDED(result)) result = g_device->CreateShaderResourceView(g_indexTexture, nullptr, &g_indexView);

    texture.Width = 256;
    texture.Height = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (SUCCEEDED(result)) result = g_device->CreateTexture2D(&texture, nullptr, &g_paletteTexture);
    if (SUCCEEDED(result)) result = g_device->CreateShaderResourceView(g_paletteTexture, nullptr, &g_paletteView);
    if (SUCCEEDED(result)) result = g_device->CreateRenderTargetView(g_backBuffer, nullptr, &g_renderTarget);
    return result;
}

void ConfigureWindowForMode() {
    if (!g_window || !g_displayWidth || !g_displayHeight) return;
    const bool menuMode = IsMenuMode();
    const HMONITOR monitor = MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoA(monitor, &info)) return;

    const DWORD style = menuMode ? WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VISIBLE : WS_POPUP | WS_VISIBLE;
    const DWORD extendedStyle = menuMode ? WS_EX_APPWINDOW : 0;
    RECT windowRect{0, 0, static_cast<LONG>(g_displayWidth), static_cast<LONG>(g_displayHeight)};
    if (menuMode && !AdjustWindowRectEx(&windowRect, style, FALSE, extendedStyle)) return;
    const RECT& bounds = menuMode ? info.rcWork : info.rcMonitor;
    const LONG windowWidth = windowRect.right - windowRect.left;
    const LONG windowHeight = windowRect.bottom - windowRect.top;
    const LONG x = bounds.left + ((bounds.right - bounds.left) - windowWidth) / 2;
    const LONG y = bounds.top + ((bounds.bottom - bounds.top) - windowHeight) / 2;

    SetWindowLongPtrA(g_window, GWL_STYLE, style);
    SetWindowLongPtrA(g_window, GWL_EXSTYLE, extendedStyle);
    SetWindowPos(g_window, nullptr, x, y, windowWidth, windowHeight,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_SHOWWINDOW);
    if (menuMode) {
        g_menuWindowX = x;
        g_menuWindowY = y;
        g_menuWindowWidth = windowWidth;
        g_menuWindowHeight = windowHeight;
        PostMessageA(g_window, kCenterMenuWindowMessage, 0, 0);
    }

    char message[160]{};
    std::snprintf(message, sizeof(message), "[GB D3D11] Window configured: %s at %ld,%ld outer=%ldx%ld client=%lux%lu.\r\n",
                  menuMode ? "movable menu" : "native borderless", x, y, windowWidth, windowHeight, g_displayWidth, g_displayHeight);
    Log(message);
}

HRESULT EnsureSwapChain() {
    if (g_swapChain) return S_OK;
    if (!g_device || !g_window || !g_displayWidth || !g_displayHeight) return DDERR_NOCOOPERATIVELEVELSET;

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    HRESULT result = g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(result)) result = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(result)) result = adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = g_displayWidth;
    description.BufferDesc.Height = g_displayHeight;
    description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = g_window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (SUCCEEDED(result)) result = factory->CreateSwapChain(g_device, &description, &g_swapChain);
    if (SUCCEEDED(result)) result = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&g_backBuffer));
    if (SUCCEEDED(result)) result = CreatePalettePipeline();

    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    if (FAILED(result)) {
        ReleaseSwapChain();
        LogResult("DXGI swap-chain creation", result);
    } else {
        Log("[GB D3D11] DXGI swap chain and palette shader created; native presentation active.\r\n");
    }
    return result;
}

class NativePalette final : public IDirectDrawPalette {
public:
    NativePalette(DWORD caps, const PALETTEENTRY* entries) : caps_(caps) {
        if (entries) std::memcpy(entries_, entries, sizeof(entries_));
        LogicalPalette256 logical{};
        logical.version = 0x0300;
        logical.count = 256;
        std::memcpy(logical.entries, entries_, sizeof(entries_));
        handle_ = CreatePalette(reinterpret_cast<LOGPALETTE*>(&logical));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirectDrawPalette) return E_NOINTERFACE;
        *object = static_cast<IDirectDrawPalette*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDWORD caps) override { if (!caps) return DDERR_INVALIDPARAMS; *caps = caps_; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY entries) override {
        if (!entries || base > 256 || count > 256 - base) return DDERR_INVALIDPARAMS;
        std::memcpy(entries, entries_ + base, count * sizeof(PALETTEENTRY)); return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE SetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY entries) override {
        if (!entries || base > 256 || count > 256 - base) return DDERR_INVALIDPARAMS;
        std::memcpy(entries_ + base, entries, count * sizeof(PALETTEENTRY));
        if (handle_) SetPaletteEntries(handle_, base, count, entries);
        ++g_paletteGeneration;
        NotifySurfaces();
        return DD_OK;
    }
    const PALETTEENTRY* Entries() const { return entries_; }
    HPALETTE Handle() const { return handle_; }
    void Attach(NativeSurface* surface) { surfaces_.push_back(surface); }
    void Detach(NativeSurface* surface) { surfaces_.erase(std::remove(surfaces_.begin(), surfaces_.end(), surface), surfaces_.end()); }
private:
    struct LogicalPalette256 { WORD version; WORD count; PALETTEENTRY entries[256]; };
    ~NativePalette() { if (handle_) DeleteObject(handle_); }
    void NotifySurfaces();
    std::atomic<ULONG> references_{1};
    DWORD caps_;
    PALETTEENTRY entries_[256]{};
    HPALETTE handle_ = nullptr;
    std::vector<NativeSurface*> surfaces_;
};

NativePalette* g_displayPalette = nullptr;
std::vector<NativeSurface*> g_nativeSurfaces;
void SetDisplayPalette(NativePalette* palette);

class NativeClipper final : public IDirectDrawClipper {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirectDrawClipper) return E_NOINTERFACE;
        *object = static_cast<IDirectDrawClipper*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetClipList(LPRECT rect, LPRGNDATA data, LPDWORD size) override {
        if (!size) return DDERR_INVALIDPARAMS;
        RECT clip{};
        if (rect) clip = *rect;
        else if (window_) GetClientRect(window_, &clip);
        else clip = clipRect_;
        const DWORD required = sizeof(RGNDATAHEADER) + sizeof(RECT);
        if (!data || *size < required) { *size = required; return data ? DDERR_REGIONTOOSMALL : DD_OK; }
        data->rdh.dwSize = sizeof(RGNDATAHEADER); data->rdh.iType = RDH_RECTANGLES;
        data->rdh.nCount = 1; data->rdh.nRgnSize = sizeof(RECT); data->rdh.rcBound = clip;
        std::memcpy(data->Buffer, &clip, sizeof(clip)); *size = required; return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHWnd(HWND* window) override { if (!window) return DDERR_INVALIDPARAMS; *window = window_; return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE IsClipListChanged(BOOL* changed) override { if (!changed) return DDERR_INVALIDPARAMS; *changed = changed_; changed_ = FALSE; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipList(LPRGNDATA data, DWORD) override {
        if (!data || !data->rdh.nCount) return DDERR_INVALIDCLIPLIST;
        clipRect_ = data->rdh.rcBound; window_ = nullptr; changed_ = TRUE; return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetHWnd(DWORD, HWND window) override { window_ = window; changed_ = TRUE; return DD_OK; }
private:
    std::atomic<ULONG> references_{1};
    HWND window_ = nullptr;
    RECT clipRect_{};
    BOOL changed_ = FALSE;
};

HRESULT PresentPrimarySurface(NativeSurface* surface, UINT syncInterval = 1);

class NativeSurface final : public IDirectDrawSurface {
public:
    NativeSurface(DWORD width, DWORD height, DWORD caps, bool primary)
        : width_(width), height_(height), pitch_((width + 3) & ~3u), caps_(caps), primary_(primary) {
        BITMAPINFO256 info{};
        info.header.biSize = sizeof(BITMAPINFOHEADER);
        info.header.biWidth = static_cast<LONG>(width_);
        info.header.biHeight = -static_cast<LONG>(height_);
        info.header.biPlanes = 1; info.header.biBitCount = 8; info.header.biCompression = BI_RGB;
        memoryDc_ = CreateCompatibleDC(nullptr);
        bitmap_ = CreateDIBSection(memoryDc_, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS,
                                   reinterpret_cast<void**>(&pixels_), nullptr, 0);
        if (bitmap_) previousBitmap_ = SelectObject(memoryDc_, bitmap_);
        g_nativeSurfaces.push_back(this);
    }
    bool Valid() const { return memoryDc_ && bitmap_ && pixels_; }
    bool Primary() const { return primary_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirectDrawSurface) return E_NOINTERFACE;
        *object = static_cast<IDirectDrawSurface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Blt(LPRECT destinationRect, LPDIRECTDRAWSURFACE sourceSurface, LPRECT sourceRect, DWORD flags, LPDDBLTFX effects) override {
        LogSampled("IDirectDrawSurface::Blt", bltCount_);
        RECT destination = destinationRect ? *destinationRect : RECT{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        HRESULT result = DD_OK;
        if (flags & DDBLT_COLORFILL) {
            if (!effects) return DDERR_INVALIDPARAMS;
            ClipRect(destination, width_, height_);
            for (LONG y = destination.top; y < destination.bottom; ++y)
                std::memset(pixels_ + y * pitch_ + destination.left, static_cast<BYTE>(effects->dwFillColor), destination.right - destination.left);
        } else {
            auto* source = dynamic_cast<NativeSurface*>(sourceSurface);
            if (!source) return DDERR_INVALIDOBJECT;
            RECT sourceArea = sourceRect ? *sourceRect : RECT{0, 0, static_cast<LONG>(source->width_), static_cast<LONG>(source->height_)};
            result = CopyRect(destination, *source, sourceArea, flags, effects);
        }
        if (primary_ && SUCCEEDED(result)) PresentPrimarySurface(this);
        return result;
    }
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE sourceSurface, LPRECT sourceRect, DWORD flags) override {
        LogSampled("IDirectDrawSurface::BltFast", bltFastCount_);
        auto* source = dynamic_cast<NativeSurface*>(sourceSurface);
        if (!source) return DDERR_INVALIDOBJECT;
        RECT sourceArea = sourceRect ? *sourceRect : RECT{0, 0, static_cast<LONG>(source->width_), static_cast<LONG>(source->height_)};
        RECT destination{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + sourceArea.right - sourceArea.left,
                         static_cast<LONG>(y) + sourceArea.bottom - sourceArea.top};
        const DWORD bltFlags = (flags & DDBLTFAST_SRCCOLORKEY) ? DDBLT_KEYSRC : 0;
        const HRESULT result = CopyRect(destination, *source, sourceArea, bltFlags, nullptr);
        if (primary_ && SUCCEEDED(result)) PresentPrimarySurface(this);
        return result;
    }
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID, LPDDENUMSURFACESCALLBACK) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE, DWORD) override { return DDERR_NOTFLIPPABLE; }
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS, LPDIRECTDRAWSURFACE*) override { return DDERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS caps) override { if (!caps) return DDERR_INVALIDPARAMS; caps->dwCaps = caps_; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER* clipper) override { if (!clipper) return DDERR_INVALIDPARAMS; *clipper = clipper_; if (clipper_) clipper_->AddRef(); return clipper_ ? DD_OK : DDERR_NOCLIPPERATTACHED; }
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD flags, LPDDCOLORKEY key) override { if (!key || !(flags & DDCKEY_SRCBLT) || !hasSourceKey_) return DDERR_NOCOLORKEY; *key = sourceKey_; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDC(HDC* dc) override {
        LogSampled("IDirectDrawSurface::GetDC", getDcCount_);
        if (!dc || dcAcquired_) return DDERR_DCALREADYCREATED;
        ApplyPalette(); dcAcquired_ = true; *dc = memoryDc_; return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DDERR_NOTFLIPPABLE; }
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE* palette) override { if (!palette) return DDERR_INVALIDPARAMS; *palette = palette_; if (palette_) palette_->AddRef(); return palette_ ? DD_OK : DDERR_NOPALETTEATTACHED; }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT format) override { if (!format) return DDERR_INVALIDPARAMS; FillPixelFormat(*format); return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC desc) override { if (!desc || desc->dwSize != sizeof(*desc)) return DDERR_INVALIDPARAMS; FillDescription(*desc, pixels_); return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Lock(LPRECT rect, LPDDSURFACEDESC desc, DWORD, HANDLE) override {
        LogSampled("IDirectDrawSurface::Lock", lockCount_);
        if (!desc || desc->dwSize != sizeof(*desc)) return DDERR_INVALIDPARAMS;
        if (locked_) return DDERR_SURFACEBUSY;
        RECT area = rect ? *rect : RECT{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        if (!ValidRect(area, width_, height_)) return DDERR_INVALIDRECT;
        FillDescription(*desc, pixels_ + area.top * pitch_ + area.left);
        desc->dwWidth = area.right - area.left; desc->dwHeight = area.bottom - area.top;
        locked_ = true; return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override { LogSampled("IDirectDrawSurface::ReleaseDC", releaseDcCount_); if (!dcAcquired_ || dc != memoryDc_) return DDERR_NODC; dcAcquired_ = false; return DD_OK; }
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER clipper) override { if (clipper) clipper->AddRef(); if (clipper_) clipper_->Release(); clipper_ = clipper; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD flags, LPDDCOLORKEY key) override { if (!(flags & DDCKEY_SRCBLT)) return DDERR_UNSUPPORTED; if (key) { sourceKey_ = *key; hasSourceKey_ = true; } else hasSourceKey_ = false; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE palette) override {
        if (palette) palette->AddRef();
        RestoreLogicalPalette();
        if (appliedPalette_) { appliedPalette_->Detach(this); appliedPalette_ = nullptr; }
        if (palette_) palette_->Release();
        palette_ = palette;
        if (primary_) SetDisplayPalette(dynamic_cast<NativePalette*>(palette_));
        ApplyPalette();
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID) override { LogSampled("IDirectDrawSurface::Unlock", unlockCount_); if (!locked_) return DDERR_NOTLOCKED; locked_ = false; return DD_OK; }
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override { return DDERR_NOTAOVERLAYSURFACE; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) override { return DDERR_NOTAOVERLAYSURFACE; }
    const BYTE* Pixels() const { return pixels_; }
    DWORD Width() const { return width_; }
    DWORD Height() const { return height_; }
    LONG Pitch() const { return static_cast<LONG>(pitch_); }
    NativePalette* Palette() const { return dynamic_cast<NativePalette*>(palette_); }
    void RefreshPalette() { ApplyPalette(); if (primary_) PresentPrimarySurface(this, 0); }
private:
    struct BITMAPINFO256 { BITMAPINFOHEADER header; RGBQUAD colors[256]; };
    ~NativeSurface() {
        g_nativeSurfaces.erase(std::remove(g_nativeSurfaces.begin(), g_nativeSurfaces.end(), this), g_nativeSurfaces.end());
        if (appliedPalette_) appliedPalette_->Detach(this);
        RestoreLogicalPalette();
        if (palette_) palette_->Release(); if (clipper_) clipper_->Release();
        if (memoryDc_ && previousBitmap_) SelectObject(memoryDc_, previousBitmap_);
        if (bitmap_) DeleteObject(bitmap_); if (memoryDc_) DeleteDC(memoryDc_);
    }
    static bool ValidRect(const RECT& rect, DWORD width, DWORD height) { return rect.left >= 0 && rect.top >= 0 && rect.right >= rect.left && rect.bottom >= rect.top && rect.right <= static_cast<LONG>(width) && rect.bottom <= static_cast<LONG>(height); }
    static void ClipRect(RECT& rect, DWORD width, DWORD height) { rect.left = std::max<LONG>(0, rect.left); rect.top = std::max<LONG>(0, rect.top); rect.right = std::min<LONG>(width, rect.right); rect.bottom = std::min<LONG>(height, rect.bottom); }
    void FillPixelFormat(DDPIXELFORMAT& format) const { std::memset(&format, 0, sizeof(format)); format.dwSize = sizeof(format); format.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8; format.dwRGBBitCount = 8; }
    void FillDescription(DDSURFACEDESC& desc, void* address) const { std::memset(&desc, 0, sizeof(desc)); desc.dwSize = sizeof(desc); desc.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE; desc.dwWidth = width_; desc.dwHeight = height_; desc.lPitch = pitch_; desc.lpSurface = address; desc.ddsCaps.dwCaps = caps_; FillPixelFormat(desc.ddpfPixelFormat); }
    void ApplyPalette() { auto* native = palette_ ? dynamic_cast<NativePalette*>(palette_) : g_displayPalette; if (!native || !memoryDc_) return; if (native != appliedPalette_) { RestoreLogicalPalette(); if (appliedPalette_) appliedPalette_->Detach(this); appliedPalette_ = native; appliedPalette_->Attach(this); } RGBQUAD colors[256]{}; const PALETTEENTRY* entries = native->Entries(); for (unsigned i = 0; i < 256; ++i) { colors[i].rgbRed = entries[i].peRed; colors[i].rgbGreen = entries[i].peGreen; colors[i].rgbBlue = entries[i].peBlue; } SetDIBColorTable(memoryDc_, 0, 256, colors); if (native->Handle()) { if (!logicalPaletteSelected_) { previousPalette_ = SelectPalette(memoryDc_, native->Handle(), FALSE); logicalPaletteSelected_ = true; } RealizePalette(memoryDc_); } }
    void RestoreLogicalPalette() { if (memoryDc_ && logicalPaletteSelected_) { SelectPalette(memoryDc_, previousPalette_, FALSE); RealizePalette(memoryDc_); previousPalette_ = nullptr; logicalPaletteSelected_ = false; } }
    HRESULT CopyRect(RECT destination, NativeSurface& source, RECT sourceArea, DWORD flags, LPDDBLTFX effects) {
        if (!ValidRect(sourceArea, source.width_, source.height_)) return DDERR_INVALIDRECT;
        const LONG requestedWidth = destination.right - destination.left, requestedHeight = destination.bottom - destination.top;
        if (requestedWidth != sourceArea.right - sourceArea.left || requestedHeight != sourceArea.bottom - sourceArea.top) return DDERR_NOSTRETCHHW;
        RECT clipped = destination; ClipRect(clipped, width_, height_);
        if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) return DD_OK;
        sourceArea.left += clipped.left - destination.left; sourceArea.top += clipped.top - destination.top;
        const bool keyed = (flags & DDBLT_KEYSRC) != 0;
        DDCOLORKEY key = source.sourceKey_;
        if (flags & DDBLT_KEYSRCOVERRIDE) { if (!effects) return DDERR_INVALIDPARAMS; key = effects->ddckSrcColorkey; }
        else if (keyed && !source.hasSourceKey_) return DDERR_NOCOLORKEY;
        const LONG width = clipped.right - clipped.left, height = clipped.bottom - clipped.top;
        for (LONG y = 0; y < height; ++y) {
            const BYTE* src = source.pixels_ + (sourceArea.top + y) * source.pitch_ + sourceArea.left;
            BYTE* dst = pixels_ + (clipped.top + y) * pitch_ + clipped.left;
            if (!keyed && !(flags & DDBLT_KEYSRCOVERRIDE)) std::memmove(dst, src, width);
            else for (LONG x = 0; x < width; ++x) if (src[x] < key.dwColorSpaceLowValue || src[x] > key.dwColorSpaceHighValue) dst[x] = src[x];
        }
        return DD_OK;
    }
    std::atomic<ULONG> references_{1};
    DWORD width_, height_, pitch_, caps_;
    bool primary_, locked_ = false, dcAcquired_ = false, hasSourceKey_ = false, logicalPaletteSelected_ = false;
    BYTE* pixels_ = nullptr; HDC memoryDc_ = nullptr; HBITMAP bitmap_ = nullptr; HGDIOBJ previousBitmap_ = nullptr;
    HPALETTE previousPalette_ = nullptr;
    NativePalette* appliedPalette_ = nullptr;
    LPDIRECTDRAWPALETTE palette_ = nullptr; LPDIRECTDRAWCLIPPER clipper_ = nullptr; DDCOLORKEY sourceKey_{};
    std::atomic<unsigned long> bltCount_{0}, bltFastCount_{0}, lockCount_{0}, unlockCount_{0}, getDcCount_{0}, releaseDcCount_{0};
};

void NativePalette::NotifySurfaces() {
    for (NativeSurface* surface : surfaces_) surface->RefreshPalette();
}

void SetDisplayPalette(NativePalette* palette) {
    if (palette == g_displayPalette) return;
    if (palette) palette->AddRef();
    NativePalette* previous = g_displayPalette;
    g_displayPalette = palette;
    ++g_paletteGeneration;
    for (NativeSurface* surface : g_nativeSurfaces) surface->RefreshPalette();
    if (previous) previous->Release();
    Log("[GB D3D11] Native display palette propagated to unpalettized CPU surfaces.\r\n");
}

HRESULT PresentPrimarySurface(NativeSurface* surface, UINT syncInterval) {
    HRESULT result = EnsureSwapChain();
    if (FAILED(result)) return result;
    NativePalette* palette = surface->Palette();
    if (!palette) return DDERR_NOPALETTEATTACHED;
    if (surface->Width() != g_displayWidth || surface->Height() != g_displayHeight) return DDERR_INVALIDPARAMS;
    const PALETTEENTRY* entries = palette->Entries();
    std::uint32_t colors[256]{};
    for (unsigned i = 0; i < 256; ++i) {
        colors[i] = static_cast<std::uint32_t>(entries[i].peRed) |
                    (static_cast<std::uint32_t>(entries[i].peGreen) << 8) |
                    (static_cast<std::uint32_t>(entries[i].peBlue) << 16) | 0xFF000000u;
    }
    g_context->UpdateSubresource(g_indexTexture, 0, nullptr, surface->Pixels(), surface->Pitch(), 0);
    g_context->UpdateSubresource(g_paletteTexture, 0, nullptr, colors, sizeof(colors), 0);

    D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(g_displayWidth), static_cast<float>(g_displayHeight), 0.0f, 1.0f};
    ID3D11ShaderResourceView* resources[] = {g_indexView, g_paletteView};
    g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
    g_context->RSSetViewports(1, &viewport);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vertexShader, nullptr, 0);
    g_context->PSSetShader(g_pixelShader, nullptr, 0);
    g_context->PSSetShaderResources(0, ARRAYSIZE(resources), resources);
    g_context->Draw(3, 0);
    result = g_swapChain->Present(syncInterval, 0);
    if (SUCCEEDED(result)) LogSampled("DXGI Present", g_presentCount); else LogResult("DXGI Present", result);
    return result;
}

class NativeDirectDraw final : public IDirectDraw {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override { if (!object) return E_POINTER; *object = nullptr; if (iid != IID_IUnknown && iid != IID_IDirectDraw) return E_NOINTERFACE; *object = static_cast<IDirectDraw*>(this); AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* output, IUnknown* outer) override { if (!output) return DDERR_INVALIDPARAMS; *output = nullptr; if (outer) return CLASS_E_NOAGGREGATION; auto* clipper = new (std::nothrow) NativeClipper(); if (!clipper) return E_OUTOFMEMORY; *output = clipper; return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreatePalette(DWORD caps, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* output, IUnknown* outer) override { if (!output || !entries) return DDERR_INVALIDPARAMS; *output = nullptr; if (outer) return CLASS_E_NOAGGREGATION; auto* palette = new (std::nothrow) NativePalette(caps, entries); if (!palette) return E_OUTOFMEMORY; *output = palette; return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* output, IUnknown* outer) override {
        if (!desc || desc->dwSize != sizeof(*desc) || !output) return DDERR_INVALIDPARAMS; *output = nullptr; if (outer) return CLASS_E_NOAGGREGATION;
        const bool primary = (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
        const DWORD width = primary && !(desc->dwFlags & DDSD_WIDTH) ? g_displayWidth : desc->dwWidth;
        const DWORD height = primary && !(desc->dwFlags & DDSD_HEIGHT) ? g_displayHeight : desc->dwHeight;
        if (!width || !height) return DDERR_INVALIDPARAMS;
        auto* surface = new (std::nothrow) NativeSurface(width, height, desc->ddsCaps.dwCaps, primary);
        if (!surface) return E_OUTOFMEMORY; if (!surface->Valid()) { surface->Release(); return DDERR_OUTOFMEMORY; }
        *output = surface; if (primary) primary_ = surface; return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD, LPDDSURFACEDESC filter, LPVOID context, LPDDENUMMODESCALLBACK callback) override { if (!callback) return DDERR_INVALIDPARAMS; const DWORD modes[][2] = {{640,480},{800,600},{1024,768},{1280,720},{1280,1024},{1920,1080}}; for (const auto& mode : modes) { if (filter && (filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != mode[0]) continue; if (filter && (filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != mode[1]) continue; DDSURFACEDESC desc{}; desc.dwSize = sizeof(desc); desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT; desc.dwWidth = mode[0]; desc.dwHeight = mode[1]; desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT); desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8; desc.ddpfPixelFormat.dwRGBBitCount = 8; if (callback(&desc, context) == DDENUMRET_CANCEL) break; } return DD_OK; }
    HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS driver, LPDDCAPS hel) override { if (driver) { const DWORD size = driver->dwSize; std::memset(driver, 0, size); driver->dwSize = size; driver->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY; driver->dwCKeyCaps = DDCKEYCAPS_SRCBLT; driver->dwVidMemTotal = driver->dwVidMemFree = 64 * 1024 * 1024; } if (hel) { const DWORD size = hel->dwSize; std::memset(hel, 0, size); hel->dwSize = size; } return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC desc) override { if (!desc || desc->dwSize != sizeof(*desc)) return DDERR_INVALIDPARAMS; std::memset(desc, 0, sizeof(*desc)); desc->dwSize = sizeof(*desc); desc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT; desc->dwWidth = g_displayWidth; desc->dwHeight = g_displayHeight; desc->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT); desc->ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8; desc->ddpfPixelFormat.dwRGBBitCount = 8; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD count, LPDWORD) override { if (!count) return DDERR_INVALIDPARAMS; *count = 0; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE* output) override { if (!output) return DDERR_INVALIDPARAMS; *output = primary_; if (!primary_) return DDERR_NOTFOUND; primary_->AddRef(); return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD frequency) override { if (!frequency) return DDERR_INVALIDPARAMS; *frequency = 60; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD) override { return DDERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL status) override { if (!status) return DDERR_INVALIDPARAMS; *status = FALSE; return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override { char message[128]{}; std::snprintf(message, sizeof(message), "[GB D3D11] IDirectDraw::SetCooperativeLevel flags=0x%08lX.\r\n", flags); Log(message); if (!window) return DDERR_INVALIDPARAMS; g_window = window; InstallMenuWindowSubclass(window); ReleaseSwapChain(); ConfigureWindowForMode(); return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD width, DWORD height, DWORD bpp) override { char message[128]{}; std::snprintf(message, sizeof(message), "[GB D3D11] IDirectDraw::SetDisplayMode %lux%lu %lu-bpp.\r\n", width, height, bpp); Log(message); if (!width || !height || bpp != 8) return DDERR_UNSUPPORTEDMODE; g_displayWidth = width; g_displayHeight = height; ReleaseSwapChain(); ConfigureWindowForMode(); return DD_OK; }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }
private:
    std::atomic<ULONG> references_{1}; NativeSurface* primary_ = nullptr;
};

bool ReplaceDirectDrawImport() {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* dll = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dll, "ddraw.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), "DirectDrawCreate") != 0) continue;
            DWORD oldProtection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), PAGE_READWRITE, &oldProtection)) return false;
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(&GbDirectDrawCreate);
            DWORD ignored = 0; VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function, sizeof(addresses->u1.Function)); return true;
        }
    }
    return false;
}

HRESULT InitializeD3D11() {
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             requested, ARRAYSIZE(requested), D3D11_SDK_VERSION, &g_device, &level, &g_context);
}

HRESULT WINAPI GbDirectDrawCreate(GUID* guid, LPDIRECTDRAW* directDraw, IUnknown* outer) {
    Log("[GB D3D11] DirectDrawCreate intercepted; ZERO FORWARDING (native IDirectDraw, original call count=0).\r\n");
    if (!directDraw) return DDERR_INVALIDPARAMS; *directDraw = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (guid) return DDERR_INVALIDDIRECTDRAWGUID;
    if (!g_device) { const HRESULT result = InitializeD3D11(); if (FAILED(result)) { LogResult("D3D11 device creation", result); return result; } Log("[GB D3D11] D3D11 device created.\r\n"); }
    auto* native = new (std::nothrow) NativeDirectDraw(); if (!native) return E_OUTOFMEMORY;
    *directDraw = native; Log("[GB D3D11] Native DirectDraw v1 object created; cnc-ddraw rendering calls=0.\r\n"); return DD_OK;
}

} // namespace

extern "C" __declspec(dllexport) BOOL __cdecl GbGetDisplayPalette(
    std::uint32_t* colors, unsigned long* generation) {
    if (!colors || !generation || !g_displayPalette) return FALSE;
    const PALETTEENTRY* entries = g_displayPalette->Entries();
    for (unsigned i = 0; i < 256; ++i) {
        colors[i] = static_cast<std::uint32_t>(entries[i].peRed) |
                    (static_cast<std::uint32_t>(entries[i].peGreen) << 8) |
                    (static_cast<std::uint32_t>(entries[i].peBlue) << 16);
    }
    *generation = g_paletteGeneration.load();
    return TRUE;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module); SetLogPath(module);
        Log("[GB D3D11] Renderer 0.9.2 loading; GPU palette shader; resizable menu window with scaled mouse input and deferred centering; zero-forwarding native backend.\r\n");
        if (!ReplaceDirectDrawImport()) { Log("[GB D3D11] DirectDrawCreate IAT replacement failed.\r\n"); return FALSE; }
        Log("[GB D3D11] DirectDrawCreate IAT replacement installed.\r\n");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_subclassedWindow && g_originalWindowProc && IsWindow(g_subclassedWindow))
            SetWindowLongPtrA(g_subclassedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWindowProc));
        ReleaseSwapChain(); if (g_context) g_context->Release(); if (g_device) g_device->Release();
    }
    return TRUE;
}
