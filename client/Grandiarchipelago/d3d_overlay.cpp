#include "d3d_overlay.h"

#include "log.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace grandia_ap {
namespace {

constexpr bool kD3dOverlayTestEnabled = true;
constexpr UINT kOverlayW = 640;
constexpr size_t kMaxToastLines = 8;
constexpr UINT kToastLineH = 30;
constexpr UINT kToastPadY = 8;
constexpr UINT kOverlayH = kToastPadY * 2 + static_cast<UINT>(kMaxToastLines) * kToastLineH;
constexpr int kOverlayX = 12;
constexpr int kOverlayY = 12;

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain* swap, UINT sync_interval, UINT flags);

void* g_present_site = nullptr;
uint8_t g_present_original[16]{};
size_t g_present_patch_size = 0;
std::mutex g_present_mutex;

struct ToastLine {
    std::wstring text;
    DWORD expire_tick = 0;  // 0 = until cleared
    unsigned rgb = 0xFFE528;
};

std::mutex g_toast_mutex;
std::vector<ToastLine> g_toasts;
bool g_toast_dirty = true;

ID3D11Device* g_cached_device = nullptr;
ID3D11Texture2D* g_overlay_tex = nullptr;
DXGI_FORMAT g_overlay_format = DXGI_FORMAT_UNKNOWN;
std::vector<uint8_t> g_pixel_scratch;  // RGBA8 for UpdateSubresource

std::atomic<bool> g_logged_first_present{false};
std::atomic<bool> g_logged_passthrough{false};
std::atomic<bool> g_logged_text_ok{false};
std::atomic<bool> g_logged_text_fail{false};

bool WriteJump(void* site, void* destination, uint8_t* original_out, size_t patch_size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    if (original_out) {
        std::memcpy(original_out, site, patch_size);
    }
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (reinterpret_cast<uint8_t*>(site) + 5));
    auto* bytes = reinterpret_cast<uint8_t*>(site);
    bytes[0] = 0xE9;
    std::memcpy(bytes + 1, &rel, sizeof(rel));
    for (size_t i = 5; i < patch_size; ++i) {
        bytes[i] = 0x90;
    }
    VirtualProtect(site, patch_size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, patch_size);
    return true;
}

void RestoreBytes(void* site, const uint8_t* original, size_t size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return;
    }
    std::memcpy(site, original, size);
    VirtualProtect(site, size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, size);
}

size_t ChoosePresentPatchSize(const uint8_t* p) {
    if (p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 && p[3] == 0x8B && p[4] == 0xEC) {
        return 5;
    }
    if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x83 && p[4] == 0xE4) {
        return 6;
    }
    if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x83 && p[4] == 0xEC) {
        return 6;
    }
    if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x81 && p[4] == 0xEC) {
        return 9;
    }
    if (p[0] == 0xE9) {
        return 5;
    }
    if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC) {
        return 5;
    }
    return 0;
}

std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), needed);
    return out;
}

void ReleaseOverlaySurface() {
    if (g_overlay_tex) {
        g_overlay_tex->Release();
        g_overlay_tex = nullptr;
    }
    g_overlay_format = DXGI_FORMAT_UNKNOWN;
}

void ReleaseDeviceResources() {
    ReleaseOverlaySurface();
    if (g_cached_device) {
        g_cached_device->Release();
        g_cached_device = nullptr;
    }
}

bool EnsureOverlayTexture(ID3D11Device* device, DXGI_FORMAT format) {
    if (g_cached_device != device) {
        ReleaseOverlaySurface();
        if (g_cached_device) {
            g_cached_device->Release();
            g_cached_device = nullptr;
        }
        g_cached_device = device;
        g_cached_device->AddRef();
        g_toast_dirty = true;
    }

    if (g_overlay_tex && g_overlay_format == format) {
        return true;
    }

    ReleaseOverlaySurface();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kOverlayW;
    td.Height = kOverlayH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    const HRESULT hr = device->CreateTexture2D(&td, nullptr, &g_overlay_tex);
    if (FAILED(hr) || !g_overlay_tex) {
        if (!g_logged_text_fail.exchange(true)) {
            LogWarn("D3D11 overlay: CreateTexture2D failed hr=0x%08X fmt=%u", static_cast<unsigned>(hr),
                    static_cast<unsigned>(format));
        }
        return false;
    }

    g_overlay_format = format;
    g_toast_dirty = true;
    return true;
}

bool PruneExpiredToastsLocked() {
    const DWORD now = GetTickCount();
    const size_t before = g_toasts.size();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(),
                                  [now](const ToastLine& line) {
                                      return line.expire_tick != 0 && now >= line.expire_tick;
                                  }),
                   g_toasts.end());
    if (g_toasts.size() != before) {
        g_toast_dirty = true;
        return true;
    }
    return false;
}

// Returns false if nothing to draw. out_lines / out_rgbs parallel; out_pixel_h = copy height.
bool GetActiveToastLines(std::vector<std::wstring>* out_lines, std::vector<unsigned>* out_rgbs,
                         UINT* out_pixel_h) {
    std::lock_guard<std::mutex> lock(g_toast_mutex);
    PruneExpiredToastsLocked();
    if (g_toasts.empty()) {
        return false;
    }
    out_lines->clear();
    out_rgbs->clear();
    out_lines->reserve(g_toasts.size());
    out_rgbs->reserve(g_toasts.size());
    for (const ToastLine& line : g_toasts) {
        out_lines->push_back(line.text);
        out_rgbs->push_back(line.rgb);
    }
    *out_pixel_h = kToastPadY * 2 + static_cast<UINT>(out_lines->size()) * kToastLineH;
    if (*out_pixel_h > kOverlayH) {
        *out_pixel_h = kOverlayH;
    }
    return true;
}

COLORREF RgbToColorRef(unsigned rgb) {
    const unsigned r = (rgb >> 16) & 0xFF;
    const unsigned g = (rgb >> 8) & 0xFF;
    const unsigned b = rgb & 0xFF;
    return RGB(r, g, b);
}

// GDI renders BGRA DIBs; game backbuffer is R8G8B8A8 — swizzle on upload.
bool RasterizeToastToRgba(const std::vector<std::wstring>& lines, const std::vector<unsigned>& rgbs,
                          UINT pixel_h, std::vector<uint8_t>* out_rgba) {
    out_rgba->assign(static_cast<size_t>(kOverlayW) * kOverlayH * 4, 0);
    if (lines.empty() || pixel_h == 0) {
        return true;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(kOverlayW);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(kOverlayH);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dib_bits = nullptr;
    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!mem || !dib || !dib_bits) {
        if (dib) {
            DeleteObject(dib);
        }
        if (mem) {
            DeleteDC(mem);
        }
        return false;
    }

    HGDIOBJ old_bmp = SelectObject(mem, dib);

    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
    RECT bg_rc = {0, 0, static_cast<LONG>(kOverlayW), static_cast<LONG>(pixel_h)};
    FillRect(mem, &bg_rc, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    HFONT font = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = font ? SelectObject(mem, font) : nullptr;

    for (size_t i = 0; i < lines.size() && i < kMaxToastLines; ++i) {
        const unsigned rgb = i < rgbs.size() ? rgbs[i] : 0xFFE528;
        SetTextColor(mem, RgbToColorRef(rgb));
        RECT text_rc = {12, static_cast<LONG>(kToastPadY + i * kToastLineH),
                        static_cast<LONG>(kOverlayW - 12),
                        static_cast<LONG>(kToastPadY + (i + 1) * kToastLineH)};
        DrawTextW(mem, lines[i].c_str(), static_cast<int>(lines[i].size()), &text_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    if (old_font) {
        SelectObject(mem, old_font);
    }
    if (font) {
        DeleteObject(font);
    }

    const auto* src = static_cast<const uint8_t*>(dib_bits);
    uint8_t* dst = out_rgba->data();
    const size_t pixels = static_cast<size_t>(kOverlayW) * kOverlayH;
    for (size_t i = 0; i < pixels; ++i) {
        const UINT y = static_cast<UINT>(i / kOverlayW);
        if (y >= pixel_h) {
            dst[i * 4 + 0] = 0;
            dst[i * 4 + 1] = 0;
            dst[i * 4 + 2] = 0;
            dst[i * 4 + 3] = 0;
            continue;
        }
        const uint8_t b = src[i * 4 + 0];
        const uint8_t g = src[i * 4 + 1];
        const uint8_t r = src[i * 4 + 2];
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = 255;
    }

    SelectObject(mem, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem);
    return true;
}

void DrawOverlayText(IDXGISwapChain* swap) {
    std::vector<std::wstring> lines;
    std::vector<unsigned> rgbs;
    UINT pixel_h = 0;
    if (!GetActiveToastLines(&lines, &rgbs, &pixel_h)) {
        return;
    }

    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(g_toast_mutex);
        dirty = g_toast_dirty;
        if (dirty) {
            g_toast_dirty = false;
        }
    }

    ID3D11Device* device = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc))) {
        device->Release();
        return;
    }

    DXGI_FORMAT format = desc.BufferDesc.Format;
    if (format == DXGI_FORMAT_UNKNOWN) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        if (!g_logged_text_fail.exchange(true)) {
            LogWarn("D3D11 overlay: unsupported backbuffer fmt=%u (need R8G8B8A8)",
                    static_cast<unsigned>(format));
        }
        device->Release();
        return;
    }

    if (!EnsureOverlayTexture(device, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        device->Release();
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        device->Release();
        return;
    }

    if (dirty) {
        if (!RasterizeToastToRgba(lines, rgbs, pixel_h, &g_pixel_scratch)) {
            if (!g_logged_text_fail.exchange(true)) {
                LogWarn("D3D11 overlay: GDI rasterize failed");
            }
            context->Release();
            device->Release();
            return;
        }
        context->UpdateSubresource(g_overlay_tex, 0, nullptr, g_pixel_scratch.data(), kOverlayW * 4,
                                   kOverlayW * kOverlayH * 4);
    }

    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) || !back_buffer) {
        context->Release();
        device->Release();
        return;
    }

    const UINT dst_w = desc.BufferDesc.Width;
    const UINT dst_h = desc.BufferDesc.Height;
    if (dst_w > static_cast<UINT>(kOverlayX) && dst_h > static_cast<UINT>(kOverlayY)) {
        const UINT copy_w = (kOverlayW < dst_w - static_cast<UINT>(kOverlayX))
                                ? kOverlayW
                                : (dst_w - static_cast<UINT>(kOverlayX));
        const UINT copy_h = (pixel_h < dst_h - static_cast<UINT>(kOverlayY))
                                ? pixel_h
                                : (dst_h - static_cast<UINT>(kOverlayY));
        D3D11_BOX src_box{};
        src_box.left = 0;
        src_box.top = 0;
        src_box.front = 0;
        src_box.right = copy_w;
        src_box.bottom = copy_h;
        src_box.back = 1;
        context->CopySubresourceRegion(back_buffer, 0, static_cast<UINT>(kOverlayX),
                                       static_cast<UINT>(kOverlayY), 0, g_overlay_tex, 0, &src_box);
    }

    back_buffer->Release();
    context->Release();
    device->Release();

    if (!g_logged_text_ok.exchange(true)) {
        LogInfo("D3D11 overlay: text path OK (GDI multi-line toast queue)");
    }
}

HRESULT __stdcall PresentHook(IDXGISwapChain* swap, UINT sync_interval, UINT flags) {
    if (swap && (flags & DXGI_PRESENT_TEST) == 0) {
        if (!g_logged_first_present.exchange(true)) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(swap->GetDesc(&desc))) {
                LogInfo("D3D11 Present hooked (first frame %ux%u hwnd=0x%p swap_effect=%u)",
                        desc.BufferDesc.Width, desc.BufferDesc.Height, desc.OutputWindow,
                        static_cast<unsigned>(desc.SwapEffect));
            } else {
                LogInfo("D3D11 Present hooked (first frame)");
            }
        }
        DrawOverlayText(swap);
    }

    HRESULT hr = E_FAIL;
    {
        std::lock_guard<std::mutex> lock(g_present_mutex);
        RestoreBytes(g_present_site, g_present_original, g_present_patch_size);
        auto* original = reinterpret_cast<PresentFn>(g_present_site);
        hr = original(swap, sync_interval, flags);
        WriteJump(g_present_site, reinterpret_cast<void*>(&PresentHook), nullptr, g_present_patch_size);
    }

    if (!g_logged_passthrough.exchange(true)) {
        LogInfo("D3D11 overlay: original Present returned hr=0x%08X (passthrough OK)",
                static_cast<unsigned>(hr));
    }
    return hr;
}

bool ResolvePresentAddress(void** out_present, size_t* out_patch_size) {
    *out_present = nullptr;
    *out_patch_size = 0;

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GrandiarchipelagoD3dProbe";
    const ATOM atom = RegisterClassW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogWarn("D3D overlay: RegisterClass failed (%lu)", GetLastError());
        return false;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        LogWarn("D3D overlay: CreateWindow failed (%lu)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap = nullptr;
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr =
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &sd, &swap, &device, &level, &context);
    if (FAILED(hr) || !swap) {
        LogWarn("D3D overlay: probe D3D11CreateDeviceAndSwapChain failed (hr=0x%08X)",
                static_cast<unsigned>(hr));
        if (context) {
            context->Release();
        }
        if (device) {
            device->Release();
        }
        DestroyWindow(hwnd);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swap);
    void* present = vtable[8];
    const auto* bytes = reinterpret_cast<const uint8_t*>(present);
    LogInfo("D3D11 Present prologue: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", bytes[0],
            bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9]);

    const size_t patch_size = ChoosePresentPatchSize(bytes);
    if (patch_size < 5 || patch_size > 16) {
        LogWarn("D3D overlay: unsupported Present prologue — refusing to patch");
        swap->Release();
        context->Release();
        device->Release();
        DestroyWindow(hwnd);
        return false;
    }

    *out_present = present;
    *out_patch_size = patch_size;

    swap->Release();
    context->Release();
    device->Release();
    DestroyWindow(hwnd);
    return true;
}

}  // namespace

void ShowD3dOverlayToast(const char* utf8_message, unsigned duration_ms, unsigned rgb) {
    ToastLine line;
    line.text = Utf8ToWide(utf8_message);
    if (line.text.empty()) {
        return;
    }
    line.expire_tick = duration_ms == 0 ? 0 : (GetTickCount() + duration_ms);
    line.rgb = rgb & 0xFFFFFFu;

    std::lock_guard<std::mutex> lock(g_toast_mutex);
    PruneExpiredToastsLocked();
    if (g_toasts.size() >= kMaxToastLines) {
        g_toasts.erase(g_toasts.begin());
    }
    g_toasts.push_back(std::move(line));
    g_toast_dirty = true;
}

void ClearD3dOverlayToast() {
    std::lock_guard<std::mutex> lock(g_toast_mutex);
    g_toasts.clear();
    g_toast_dirty = true;
}

bool InstallD3dOverlay() {
    if (!kD3dOverlayTestEnabled) {
        return false;
    }
    if (g_present_site) {
        return true;
    }

    void* present = nullptr;
    size_t patch_size = 0;
    if (!ResolvePresentAddress(&present, &patch_size)) {
        return false;
    }

    g_present_site = present;
    g_present_patch_size = patch_size;
    if (!WriteJump(g_present_site, reinterpret_cast<void*>(&PresentHook), g_present_original,
                   patch_size)) {
        LogWarn("D3D overlay: failed to patch Present");
        g_present_site = nullptr;
        g_present_patch_size = 0;
        return false;
    }

    ShowD3dOverlayToast("Grandiarchipelago overlay ready", 8000);
    LogInfo("D3D11 overlay installed (Present@0x%08X, patch=%zu, GDI toast)",
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(present)), patch_size);
    return true;
}

void ShutdownD3dOverlay() {
    {
        std::lock_guard<std::mutex> lock(g_present_mutex);
        if (g_present_site && g_present_patch_size) {
            RestoreBytes(g_present_site, g_present_original, g_present_patch_size);
            g_present_site = nullptr;
            g_present_patch_size = 0;
        }
    }
    ReleaseDeviceResources();
    ClearD3dOverlayToast();
}

bool IsD3dOverlayInstalled() {
    return g_present_site != nullptr;
}

}  // namespace grandia_ap
