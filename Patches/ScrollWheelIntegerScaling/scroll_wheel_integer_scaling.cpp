#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace {

constexpr std::uintptr_t kDrawViewCall = 0x0060BC64;
constexpr std::uintptr_t kDrawView = 0x0060BF40;
constexpr std::uintptr_t kPeekMessageCall = 0x00425EEF;
constexpr std::uintptr_t kPeekMessageIat = 0x00654348;
constexpr std::uintptr_t kFinalizeViewFrame = 0x0060BC6E;
constexpr std::uintptr_t kSurfaceLock = 0x00472A70;
constexpr std::uintptr_t kSurfaceUnlock = 0x00472B20;
constexpr std::uintptr_t kApplicationInstance = 0x006A3684;
constexpr std::size_t kPopupActiveOffset = 0xB44;
constexpr UINT kMaximumScale = 2;
constexpr DWORD kZoomTransitionMilliseconds = 180;
constexpr std::uint32_t kScaleOne = 1u << 16;

using PeekMessageAProc = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using DrawViewProc = void (__thiscall*)(void*, int, void*);
using SurfaceLockProc = int (__thiscall*)(void*, const char*, int);
using SurfaceUnlockProc = void (__thiscall*)(void*, const char*);

PeekMessageAProc g_peekMessageA = nullptr;
UINT g_targetScale = 1;
std::uint32_t g_transitionStartScale = kScaleOne;
DWORD g_transitionStartTime = 0;
std::atomic<LONG> g_focusX{-1};
std::atomic<LONG> g_focusY{-1};
std::atomic<LONG> g_viewLeft{0};
std::atomic<LONG> g_viewTop{0};
std::atomic<LONG> g_viewWidth{0};
std::atomic<LONG> g_viewHeight{0};
char g_logPath[MAX_PATH]{};
bool g_zoomDirtiedWorld = false;
bool g_lastWorldFrameWasFull = false;
std::atomic<std::uint32_t> g_appliedScale{kScaleOne};
std::atomic<LONG> g_appliedOriginX{0};
std::atomic<LONG> g_appliedOriginY{0};

BOOL WINAPI HookedPeekMessageA(LPMSG message, HWND window, UINT minimum, UINT maximum, UINT remove);

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

void SetLogPath(HMODULE module) {
    const DWORD length = GetModuleFileNameA(module, g_logPath, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    char* name = g_logPath + length;
    while (name != g_logPath && name[-1] != '\\' && name[-1] != '/') --name;
    lstrcpyA(name, "gb-scroll-wheel-gameplay-zoom.log");
}

std::uint32_t CurrentScale() {
    const DWORD elapsed = GetTickCount() - g_transitionStartTime;
    const std::uint32_t target = g_targetScale * kScaleOne;
    if (elapsed >= kZoomTransitionMilliseconds) return target;
    const std::uint32_t t = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(elapsed) * kScaleOne / kZoomTransitionMilliseconds);
    const std::uint32_t eased = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(t) * t / kScaleOne
        * (3 * kScaleOne - 2 * t) / kScaleOne);
    const std::int64_t distance = static_cast<std::int64_t>(target) - g_transitionStartScale;
    return static_cast<std::uint32_t>(
        static_cast<std::int64_t>(g_transitionStartScale)
        + distance * eased / kScaleOne);
}

LONG CalculateOrigin(LONG focus, LONG extent, LONG cropExtent, std::uint32_t scale) {
    const LONG sampledFocus = static_cast<LONG>(
        static_cast<std::int64_t>(focus) * kScaleOne / scale);
    return std::clamp(focus - sampledFocus, 0L, extent - cropExtent);
}

bool PatchRelativeCall(std::uintptr_t address, const void* replacement) {
    auto* instruction = reinterpret_cast<std::uint8_t*>(address);
    if (instruction[0] != 0xE8) return false;
    const auto expected = static_cast<std::int32_t>(kDrawView - (kDrawViewCall + 5));
    std::int32_t current = 0;
    std::memcpy(&current, instruction + 1, sizeof(current));
    if (current != expected) return false;

    const auto target = reinterpret_cast<std::uintptr_t>(replacement);
    const auto displacement = static_cast<std::int32_t>(target - (address + 5));
    DWORD oldProtection = 0;
    if (!VirtualProtect(instruction, 5, PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    std::memcpy(instruction + 1, &displacement, sizeof(displacement));
    DWORD ignored = 0;
    VirtualProtect(instruction, 5, oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), instruction, 5);
    return true;
}

bool PatchPeekMessageCall() {
    auto* instruction = reinterpret_cast<std::uint8_t*>(kPeekMessageCall);
    static constexpr std::uint8_t expected[] = {0xFF, 0x15, 0x48, 0x43, 0x65, 0x00};
    if (std::memcmp(instruction, expected, sizeof(expected)) != 0) return false;
    g_peekMessageA = *reinterpret_cast<PeekMessageAProc*>(kPeekMessageIat);
    if (!g_peekMessageA) return false;

    const auto target = reinterpret_cast<std::uintptr_t>(&HookedPeekMessageA);
    const auto displacement = static_cast<std::int32_t>(target - (kPeekMessageCall + 5));
    DWORD oldProtection = 0;
    if (!VirtualProtect(instruction, sizeof(expected), PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    instruction[0] = 0xE8;
    std::memcpy(instruction + 1, &displacement, sizeof(displacement));
    instruction[5] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(instruction, sizeof(expected), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), instruction, sizeof(expected));
    return true;
}

void __fastcall FinalizeWorldFrameImpl(void* view, void*) {
    auto* incremental = reinterpret_cast<std::uint8_t*>(view) + 0x146;
    if (g_targetScale > 1 || CurrentScale() > kScaleOne) {
        g_zoomDirtiedWorld = true;
        *incremental = 0;
        return;
    }
    if (g_zoomDirtiedWorld && !g_lastWorldFrameWasFull) {
        *incremental = 0;
        return;
    }
    g_zoomDirtiedWorld = false;
    *incremental = 1;
}

__declspec(naked) void FinalizeWorldFrameThunk() {
    __asm {
        pushfd
        pushad
        call FinalizeWorldFrameImpl
        popad
        popfd
        ret
    }
}

bool PatchViewFrameFinalizer() {
    auto* instruction = reinterpret_cast<std::uint8_t*>(kFinalizeViewFrame);
    static constexpr std::uint8_t expected[] = {0xC6, 0x85, 0x46, 0x01, 0x00, 0x00, 0x01};
    if (std::memcmp(instruction, expected, sizeof(expected)) != 0) return false;
    const auto target = reinterpret_cast<std::uintptr_t>(&FinalizeWorldFrameThunk);
    const auto displacement = static_cast<std::int32_t>(target - (kFinalizeViewFrame + 5));
    DWORD oldProtection = 0;
    if (!VirtualProtect(instruction, sizeof(expected), PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    instruction[0] = 0xE8;
    std::memcpy(instruction + 1, &displacement, sizeof(displacement));
    instruction[5] = 0x90;
    instruction[6] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(instruction, sizeof(expected), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), instruction, sizeof(expected));
    return true;
}

bool IsPopupActive() {
    const auto application = *reinterpret_cast<std::uintptr_t*>(kApplicationInstance);
    return application
        && *reinterpret_cast<const std::uint32_t*>(application + kPopupActiveOffset) != 0;
}

void ResetZoomForPopup() {
    if (g_targetScale == 1 && CurrentScale() <= kScaleOne) return;
    g_targetScale = 1;
    g_transitionStartScale = kScaleOne;
    g_transitionStartTime = GetTickCount() - kZoomTransitionMilliseconds;
    Log("[GB Gameplay Zoom] Reset to 1x for popup UI.\r\n");
}

void ScaleView(void* view) {
    if (IsPopupActive()) ResetZoomForPopup();
    const std::uint32_t scale = CurrentScale();
    auto* fields = static_cast<std::uint32_t*>(view);
    const LONG left = static_cast<LONG>(fields[3]);
    const LONG top = static_cast<LONG>(fields[4]);
    const LONG width = static_cast<LONG>(fields[5]);
    const LONG height = static_cast<LONG>(fields[6]);
    g_viewLeft.store(left, std::memory_order_relaxed);
    g_viewTop.store(top, std::memory_order_relaxed);
    g_viewWidth.store(width, std::memory_order_relaxed);
    g_viewHeight.store(height, std::memory_order_relaxed);
    g_appliedScale.store(kScaleOne, std::memory_order_release);
    g_appliedOriginX.store(0, std::memory_order_relaxed);
    g_appliedOriginY.store(0, std::memory_order_relaxed);
    if (scale <= kScaleOne || width <= 0 || height <= 0) return;

    void* surface = reinterpret_cast<void*>(fields[8]);
    if (!surface) return;
    const auto lock = reinterpret_cast<SurfaceLockProc>(kSurfaceLock);
    const auto unlock = reinterpret_cast<SurfaceUnlockProc>(kSurfaceUnlock);
    static constexpr char label[] = "scroll_zoom::scale_view";
    if (!lock(surface, label, 1)) return;

    auto* surfaceFields = static_cast<std::uint32_t*>(surface);
    auto** rows = reinterpret_cast<std::uint8_t**>(surfaceFields[0x30]);
    const LONG surfaceWidth = static_cast<LONG>(surfaceFields[6]);
    const LONG surfaceHeight = static_cast<LONG>(surfaceFields[7]);
    if (!rows || left < 0 || top < 0 || left + width > surfaceWidth || top + height > surfaceHeight) {
        unlock(surface, label);
        return;
    }

    static thread_local std::vector<std::uint8_t> source;
    source.resize(static_cast<std::size_t>(width) * height);
    for (LONG y = 0; y < height; ++y) {
        std::memcpy(source.data() + static_cast<std::size_t>(y) * width, rows[top + y] + left, width);
    }

    LONG focusX = g_focusX.load(std::memory_order_relaxed) - left;
    LONG focusY = g_focusY.load(std::memory_order_relaxed) - top;
    if (focusX < 0 || focusX >= width) focusX = width / 2;
    if (focusY < 0 || focusY >= height) focusY = height / 2;
    const LONG cropWidth = std::max<LONG>(1, static_cast<LONG>(
        static_cast<std::int64_t>(width) * kScaleOne / scale));
    const LONG cropHeight = std::max<LONG>(1, static_cast<LONG>(
        static_cast<std::int64_t>(height) * kScaleOne / scale));
    const LONG originX = CalculateOrigin(focusX, width, cropWidth, scale);
    const LONG originY = CalculateOrigin(focusY, height, cropHeight, scale);

    for (LONG y = 0; y < height; ++y) {
        const LONG sourceY = std::min(originY + static_cast<LONG>(
            static_cast<std::int64_t>(y) * kScaleOne / scale), height - 1);
        auto* destination = rows[top + y] + left;
        const auto* sourceRow = source.data() + static_cast<std::size_t>(sourceY) * width;
        for (LONG x = 0; x < width; ++x) {
            destination[x] = sourceRow[std::min(originX + static_cast<LONG>(
                static_cast<std::int64_t>(x) * kScaleOne / scale), width - 1)];
        }
    }
    g_appliedOriginX.store(originX, std::memory_order_relaxed);
    g_appliedOriginY.store(originY, std::memory_order_relaxed);
    g_appliedScale.store(scale, std::memory_order_release);
    unlock(surface, label);
}

void __fastcall HookedDrawView(void* view, void*, int mode, void* surface) {
    g_lastWorldFrameWasFull = *(reinterpret_cast<std::uint8_t*>(view) + 0x146) == 0;
    reinterpret_cast<DrawViewProc>(kDrawView)(view, mode, surface);
    ScaleView(view);
}

void RemapPoint(MSG* message) {
    const std::uint32_t scale = CurrentScale();
    if (scale <= kScaleOne) return;
    const LONG left = g_viewLeft.load(std::memory_order_relaxed);
    const LONG top = g_viewTop.load(std::memory_order_relaxed);
    const LONG width = g_viewWidth.load(std::memory_order_relaxed);
    const LONG height = g_viewHeight.load(std::memory_order_relaxed);
    LONG x = GET_X_LPARAM(message->lParam);
    LONG y = GET_Y_LPARAM(message->lParam);
    if (x < left || y < top || x >= left + width || y >= top + height) return;
    const LONG cropWidth = std::max<LONG>(1, static_cast<LONG>(
        static_cast<std::int64_t>(width) * kScaleOne / scale));
    const LONG cropHeight = std::max<LONG>(1, static_cast<LONG>(
        static_cast<std::int64_t>(height) * kScaleOne / scale));
    const LONG focusX = std::clamp(g_focusX.load() - left, 0L, width - 1);
    const LONG focusY = std::clamp(g_focusY.load() - top, 0L, height - 1);
    const LONG originX = CalculateOrigin(focusX, width, cropWidth, scale);
    const LONG originY = CalculateOrigin(focusY, height, cropHeight, scale);
    x = left + originX + static_cast<LONG>(
        static_cast<std::int64_t>(x - left) * kScaleOne / scale);
    y = top + originY + static_cast<LONG>(
        static_cast<std::int64_t>(y - top) * kScaleOne / scale);
    message->lParam = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
}

BOOL WINAPI HookedPeekMessageA(LPMSG message, HWND window, UINT minimum, UINT maximum, UINT remove) {
    const BOOL result = g_peekMessageA(message, window, minimum, maximum, remove);
    if (!result || !message) return result;
    if (IsPopupActive()) {
        ResetZoomForPopup();
        if (message->message == WM_MOUSEWHEEL) message->message = WM_NULL;
        return result;
    }
    if (message->message == WM_MOUSEWHEEL) {
        const short delta = GET_WHEEL_DELTA_WPARAM(message->wParam);
        const UINT scale = delta > 0 ? std::min(g_targetScale + 1, kMaximumScale)
                                     : (delta < 0 ? std::max(g_targetScale - 1, 1u) : g_targetScale);
        if (scale != g_targetScale) {
            if (scale > g_targetScale) {
                POINT point{GET_X_LPARAM(message->lParam), GET_Y_LPARAM(message->lParam)};
                if (message->hwnd) ScreenToClient(message->hwnd, &point);
                g_focusX.store(point.x, std::memory_order_relaxed);
                g_focusY.store(point.y, std::memory_order_relaxed);
            }
            g_transitionStartScale = CurrentScale();
            g_targetScale = scale;
            g_transitionStartTime = GetTickCount();
            char line[96]{};
            std::snprintf(line, sizeof(line), "[GB Gameplay Zoom] Scale changed to %ux.\r\n", scale);
            Log(line);
        }
        message->message = WM_NULL;
    } else if (message->message >= WM_MOUSEFIRST && message->message <= WM_MOUSELAST) {
        RemapPoint(message);
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) BOOL __cdecl GbGetGameplayZoomTransform(
    long* left, long* top, long* width, long* height, std::uint32_t* scale,
    long* originX, long* originY) {
    if (!left || !top || !width || !height || !scale || !originX || !originY) return FALSE;
    *left = g_viewLeft.load(std::memory_order_relaxed);
    *top = g_viewTop.load(std::memory_order_relaxed);
    *width = g_viewWidth.load(std::memory_order_relaxed);
    *height = g_viewHeight.load(std::memory_order_relaxed);
    *originX = g_appliedOriginX.load(std::memory_order_relaxed);
    *originY = g_appliedOriginY.load(std::memory_order_relaxed);
    *scale = g_appliedScale.load(std::memory_order_acquire);
    return TRUE;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        SetLogPath(module);
        const bool inputHook = PatchPeekMessageCall();
        const bool viewHook = PatchRelativeCall(kDrawViewCall, &HookedDrawView);
        const bool finalizerHook = PatchViewFrameFinalizer();
        Log(inputHook && viewHook && finalizerHook
                ? "[GB Gameplay Zoom] Wheel and view hooks installed.\r\n"
                : "[GB Gameplay Zoom] Hook installation failed; executable version is not supported.\r\n");
    }
    return TRUE;
}
