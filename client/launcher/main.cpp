// Win32 DLL injector for grandia.exe with LoadLibrary verification.

#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const bool ok = GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

DWORD FindProcessId(const wchar_t* process_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return 0;
    }

    do {
        if (_wcsicmp(entry.szExeFile, process_name) == 0) {
            const DWORD pid = entry.th32ProcessID;
            CloseHandle(snapshot);
            return pid;
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return 0;
}

bool IsProcessWow64(HANDLE process) {
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64)) {
        return false;
    }
    return wow64 != FALSE;
}

bool InjectDll(DWORD process_id, const std::wstring& dll_path, DWORD* out_load_library_result) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, process_id);
    if (!process) {
        std::fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return false;
    }

    const size_t size_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_memory =
        VirtualAllocEx(process, nullptr, size_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_memory) {
        std::fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remote_memory, dll_path.c_str(), size_bytes, nullptr)) {
        std::fprintf(stderr, "WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote_memory, 0, nullptr);
    if (!thread) {
        std::fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
    CloseHandle(process);

    if (out_load_library_result) {
        *out_load_library_result = exit_code;
    }
    return exit_code != 0;
}

std::wstring GetAbsolutePath(const wchar_t* path) {
    wchar_t buffer[MAX_PATH];
    if (GetFullPathNameW(path, MAX_PATH, buffer, nullptr) == 0) {
        return path;
    }
    return buffer;
}

void PrintUsage() {
    std::puts("GrandiaLauncher — inject Grandiarchipelago.dll into grandia.exe");
    std::puts("Usage:");
    std::puts("  GrandiaLauncher.exe [path\\to\\Grandiarchipelago.dll]");
    std::puts("");
    std::puts("Requirements:");
    std::puts("  - Game must be running IN-GAME (grandia.exe process, not just the launcher menu)");
    std::puts("  - DLL must be 32-bit (Win32 build)");
    std::puts("");
    std::puts("Log file is written next to grandia.exe: Grandiarchipelago.log");
}

const char* LoadLibraryErrorHint(DWORD exit_code) {
    switch (exit_code) {
        case 0:
            return "LoadLibraryW returned NULL (bad path, wrong bitness, or missing VC++ runtime)";
        case 193:
            return "ERROR_BAD_EXE_FORMAT — DLL bitness does not match grandia.exe (need Win32/x86 DLL)";
        default:
            return "See Windows error code above";
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && (wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"--help") == 0)) {
        PrintUsage();
        return 0;
    }

    EnableDebugPrivilege();

    const std::wstring dll_path = (argc > 1) ? GetAbsolutePath(argv[1])
                                             : GetAbsolutePath(L"Grandiarchipelago.dll");

    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"DLL not found: %s\n", dll_path.c_str());
        return 1;
    }

    const DWORD pid = FindProcessId(L"grandia.exe");
    if (!pid) {
        std::fprintf(stderr,
                     "grandia.exe not found.\n"
                     "Launch the game and load into the world (past menus). Steam often keeps\n"
                     "launcher.exe on the title screen — injection needs the running grandia.exe process.\n");
        return 1;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process) {
        if (!IsProcessWow64(process)) {
            std::fprintf(stderr,
                         "Warning: grandia.exe is not a WoW64 process from this launcher's view.\n"
                         "Ensure Grandiarchipelago.dll was built with cmake -A Win32.\n");
        }
        CloseHandle(process);
    }

    std::wprintf(L"Injecting %s into grandia.exe (pid %lu)\n", dll_path.c_str(), pid);

    DWORD load_result = 0;
    if (!InjectDll(pid, dll_path, &load_result)) {
        std::fprintf(stderr, "Injection failed. LoadLibrary result=0x%08lX (%lu)\n", load_result, load_result);
        std::fprintf(stderr, "Hint: %s\n", LoadLibraryErrorHint(load_result));
        return 1;
    }

    std::printf("Injection succeeded (module handle=0x%08lX).\n", load_result);
    std::puts("Check Grandiarchipelago.log in your Grandia install folder (same folder as grandia.exe).");
    return 0;
}
