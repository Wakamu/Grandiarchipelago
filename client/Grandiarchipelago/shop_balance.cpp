#include "shop_balance.h"

#include "game_memory.h"
#include "log.h"
#include "m_dat_balance.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace grandia_ap {

namespace {

using FopenFn = FILE*(__cdecl*)(const char* path, const char* mode);

constexpr std::uintptr_t kFopenIatRva = 0x1FE40Cu;  // VA 0x5FE40C @ image base 0x400000

FopenFn g_orig_fopen = nullptr;
void** g_fopen_iat_slot = nullptr;
FopenFn g_fopen_iat_original = nullptr;
std::string g_overlay_root;  // …/redux_content  (FIELD/, BIN/, BATLE/, TEXT/)

std::string Basename(const char* path) {
    if (!path || !*path) {
        return {};
    }
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    return base;
}

std::string Dirname(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return ".";
    }
    return path.substr(0, slash);
}

std::string ModuleDirectory() {
    char buf[MAX_PATH]{};
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ModuleDirectory), &self)) {
        return {};
    }
    if (!GetModuleFileNameA(self, buf, MAX_PATH)) {
        return {};
    }
    return Dirname(buf);
}

bool FileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string ToUpperAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

std::string NormalizeSlashes(std::string s) {
    for (char& c : s) {
        if (c == '/') {
            c = '\\';
        }
    }
    return s;
}

std::string ResolveOverlayRoot() {
    char env[MAX_PATH]{};
    if (GetEnvironmentVariableA("GRANDIA_REDUX_CONTENT", env, MAX_PATH) > 0) {
        // Env may point at content/ (has FIELD/) or at redux_content/.
        std::string root = env;
        if (DirExists(root + "\\FIELD") || DirExists(root + "\\BIN")) {
            return root;
        }
        if (DirExists(root + "\\redux_content\\FIELD")) {
            return root + "\\redux_content";
        }
    }

    const std::string dll_dir = ModuleDirectory();
    if (dll_dir.empty()) {
        return {};
    }

    const char* candidates[] = {
        "\\redux_content",
        "\\..\\..\\..\\worlds\\grandia\\native\\redux_content",
        "\\..\\..\\..\\data\\redux_spike\\redux_0.4.9\\GrandiaRemasteredRedux_V0.4.9\\content",
    };
    for (const char* rel : candidates) {
        std::string root = dll_dir + rel;
        if (DirExists(root + "\\FIELD") || DirExists(root + "\\BIN")) {
            return root;
        }
    }
    return {};
}

// True if basename is a Redux-overlay candidate (even when we have no file yet).
bool LooksLikeOverlayAsset(const std::string& base) {
    if (base.empty()) {
        return false;
    }
    const char* dot = strrchr(base.c_str(), '.');
    if (!dot) {
        return false;
    }
    if (_stricmp(dot, ".BIN") == 0) {
        return _stricmp(base.c_str(), "SHOP.BIN") == 0 || _stricmp(base.c_str(), "ITEM.BIN") == 0 ||
               _stricmp(base.c_str(), "FWIN.BIN") == 0 || _stricmp(base.c_str(), "WINDT.BIN") == 0 ||
               _stricmp(base.c_str(), "M_DAT.BIN") == 0 || _stricmp(base.c_str(), "TEXT1.BIN") == 0;
    }
    if (_stricmp(dot, ".DAT") == 0) {
        return _stricmp(base.c_str(), "MCHAR.DAT") == 0;
    }
    if (_stricmp(dot, ".TXT") == 0) {
        return _stricmp(base.c_str(), "strings.txt") == 0;
    }
    if (_stricmp(dot, ".MDP") == 0 || _stricmp(dot, ".SCN") == 0 || _stricmp(dot, ".BBG") == 0) {
        return true;
    }
    return false;
}

// Extract content-relative suffix: FIELD\…, BIN\…, BATLE\…, TEXT\LANG\…
std::string ContentRelativeSuffix(const char* original_path) {
    if (!original_path || !*original_path) {
        return {};
    }
    std::string norm = NormalizeSlashes(original_path);
    std::string upper = ToUpperAscii(norm);

    const char* markers[] = {"\\FIELD\\", "\\BIN\\", "\\BATLE\\", "\\TEXT\\"};
    size_t best = std::string::npos;
    size_t marker_len = 0;
    for (const char* m : markers) {
        const size_t pos = upper.rfind(m);
        if (pos != std::string::npos && (best == std::string::npos || pos > best)) {
            best = pos;
            marker_len = 1;  // skip leading '\'
        }
    }
    if (best == std::string::npos) {
        // Bare basename like "MCHAR.DAT" / "204C.MDP"
        return {};
    }
    // Keep original casing from norm, drop leading slash.
    return norm.substr(best + marker_len);
}

bool TryOverlayFile(const std::string& root, const std::string& rel, std::string* out) {
    if (rel.empty()) {
        return false;
    }
    std::string p = root + "\\" + NormalizeSlashes(rel);
    if (FileExists(p)) {
        *out = p;
        return true;
    }
    // Uppercase filename fallback (Redux ships UPPER names).
    const auto slash = p.find_last_of('\\');
    if (slash != std::string::npos) {
        std::string dir = p.substr(0, slash);
        std::string base = ToUpperAscii(p.substr(slash + 1));
        p = dir + "\\" + base;
        if (FileExists(p)) {
            *out = p;
            return true;
        }
    }
    return false;
}

std::string OverlayPathFor(const char* original_path) {
    if (g_overlay_root.empty()) {
        g_overlay_root = ResolveOverlayRoot();
    }
    if (g_overlay_root.empty()) {
        return {};
    }

    std::string found;
    const std::string rel = ContentRelativeSuffix(original_path);
    if (!rel.empty() && TryOverlayFile(g_overlay_root, rel, &found)) {
        return found;
    }

    // Do not guess TEXT/EN for bare *.SCN / strings — non-EN locales must keep vanilla.
    const std::string base = Basename(original_path);
    if (!LooksLikeOverlayAsset(base)) {
        return {};
    }
    const char* dot = strrchr(base.c_str(), '.');
    if (dot && (_stricmp(dot, ".SCN") == 0 || _stricmp(dot, ".TXT") == 0)) {
        return {};
    }
    const std::string upper = ToUpperAscii(base);
    const char* guess_dirs[3] = {};
    size_t guess_n = 0;
    if (dot && _stricmp(dot, ".DAT") == 0) {
        guess_dirs[guess_n++] = "BIN";
    } else if (dot && _stricmp(dot, ".BBG") == 0) {
        guess_dirs[guess_n++] = "BATLE";
    } else if (dot && _stricmp(dot, ".BIN") == 0 && _stricmp(base.c_str(), "M_DAT.BIN") == 0) {
        guess_dirs[guess_n++] = "BATLE";
    } else if (dot && _stricmp(dot, ".BIN") == 0 && _stricmp(base.c_str(), "TEXT1.BIN") == 0) {
        guess_dirs[guess_n++] = "TEXT\\EN";
    } else {
        guess_dirs[guess_n++] = "FIELD";
    }
    for (size_t i = 0; i < guess_n; ++i) {
        if (TryOverlayFile(g_overlay_root, std::string(guess_dirs[i]) + "\\" + upper, &found)) {
            return found;
        }
        if (TryOverlayFile(g_overlay_root, std::string(guess_dirs[i]) + "\\" + base, &found)) {
            return found;
        }
    }
    return {};
}

FILE* __cdecl ApFopenHook(const char* path, const char* mode) {
    if (GetGameplayBalance() == 1 && path) {
        const std::string overlay = OverlayPathFor(path);
        if (!overlay.empty()) {
            FILE* f = g_orig_fopen(overlay.c_str(), mode);
            if (f) {
                return f;
            }
            g_overlay_root.clear();
        }
    }
    return g_orig_fopen(path, mode);
}

bool PatchIatSlot(void** slot, void* detour, void** original_out) {
    if (!slot || !detour) {
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    if (original_out) {
        *original_out = *slot;
    }
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

bool InstallShopBalanceHooks() {
    if (g_fopen_iat_slot) {
        return true;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }

    g_overlay_root = ResolveOverlayRoot();

    auto* slot = reinterpret_cast<void**>(base + kFopenIatRva);
    if (!*slot) {
        LogWarn("Redux content: fopen IAT slot empty");
        return false;
    }

    if (!PatchIatSlot(slot, reinterpret_cast<void*>(&ApFopenHook),
                      reinterpret_cast<void**>(&g_fopen_iat_original))) {
        LogWarn("Redux content: failed to patch fopen IAT");
        return false;
    }
    g_fopen_iat_slot = slot;
    g_orig_fopen = g_fopen_iat_original;
    return true;
}

void RemoveShopBalanceHooks() {
    if (g_fopen_iat_slot && g_fopen_iat_original) {
        PatchIatSlot(g_fopen_iat_slot, reinterpret_cast<void*>(g_fopen_iat_original), nullptr);
    }
    g_fopen_iat_slot = nullptr;
    g_fopen_iat_original = nullptr;
    g_orig_fopen = nullptr;
    g_overlay_root.clear();
}

bool IsShopBalanceHookInstalled() {
    return g_fopen_iat_slot != nullptr;
}

}  // namespace grandia_ap
