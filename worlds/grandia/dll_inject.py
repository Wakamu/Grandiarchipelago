"""Inject Grandiarchipelago.dll into a running 32-bit grandia.exe.

Archipelago's Python is usually 64-bit, so we resolve LoadLibraryW from the
*target* process's kernel32 export table (same approach as FF7pelago).
"""

from __future__ import annotations

import ctypes
import logging
import struct
from ctypes import wintypes
from pathlib import Path
from typing import Optional

logger = logging.getLogger("GrandiaClient")

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)

_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.CloseHandle.restype = wintypes.BOOL

_k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_k32.OpenProcess.restype = wintypes.HANDLE

_k32.GetProcessId.argtypes = [wintypes.HANDLE]
_k32.GetProcessId.restype = wintypes.DWORD

_k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
_k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE

_k32.VirtualAllocEx.restype = wintypes.LPVOID
_k32.VirtualAllocEx.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    ctypes.c_size_t,
    wintypes.DWORD,
    wintypes.DWORD,
]
_k32.VirtualFreeEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD]
_k32.VirtualFreeEx.restype = wintypes.BOOL

_k32.WriteProcessMemory.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.LPCVOID,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_k32.WriteProcessMemory.restype = wintypes.BOOL

_k32.ReadProcessMemory.argtypes = [
    wintypes.HANDLE,
    wintypes.LPCVOID,
    wintypes.LPVOID,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_k32.ReadProcessMemory.restype = wintypes.BOOL

_k32.CreateRemoteThread.restype = wintypes.HANDLE
_k32.CreateRemoteThread.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    ctypes.c_size_t,
    wintypes.LPVOID,
    wintypes.LPVOID,
    wintypes.DWORD,
    wintypes.LPVOID,
]
_k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_k32.WaitForSingleObject.restype = wintypes.DWORD
_k32.GetExitCodeThread.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
_k32.GetExitCodeThread.restype = wintypes.BOOL

_MEM_COMMIT_RESERVE = 0x3000
_PAGE_READWRITE = 0x04
_PAGE_EXECUTE_READWRITE = 0x40
_PROCESS_ALL = 0x1FFFFF
_TH32CS_SNAPPROCESS = 0x00000002
_TH32CS_SNAPMODULE = 0x00000008
_TH32CS_SNAPMODULE32 = 0x00000010
_INVALID = ctypes.c_void_p(-1).value

_IMAGE_FILE_MACHINE_I386 = 0x014C
_IMAGE_FILE_MACHINE_AMD64 = 0x8664

_WIN32_ERROR_HINTS = {
    2: "ERROR_FILE_NOT_FOUND — DLL path not visible to grandia.exe",
    3: "ERROR_PATH_NOT_FOUND — DLL folder missing from game process view",
    5: "ERROR_ACCESS_DENIED — antivirus / permissions blocking the load",
    126: "ERROR_MOD_NOT_FOUND — missing dependency (or DLL deleted after extract)",
    127: "ERROR_PROC_NOT_FOUND — DLL export mismatch",
    193: "ERROR_BAD_EXE_FORMAT — need Win32/x86 Grandiarchipelago.dll (not x64)",
    1114: "ERROR_DLL_INIT_FAILED — DllMain failed inside the game process",
    0xFFFFFFFF: "LoadLibrary failed with GetLastError=0 (often AV quarantine)",
}


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", wintypes.HMODULE),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * 260),
    ]


_k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
_k32.Process32FirstW.restype = wintypes.BOOL
_k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
_k32.Process32NextW.restype = wintypes.BOOL
_k32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
_k32.Module32FirstW.restype = wintypes.BOOL
_k32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
_k32.Module32NextW.restype = wintypes.BOOL


def find_process_id(process_name: str = "grandia.exe") -> Optional[int]:
    snap = _k32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if not snap or snap == _INVALID:
        return None
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    try:
        if not _k32.Process32FirstW(snap, ctypes.byref(entry)):
            return None
        target = process_name.lower()
        while True:
            if entry.szExeFile.lower() == target:
                return int(entry.th32ProcessID)
            if not _k32.Process32NextW(snap, ctypes.byref(entry)):
                break
    finally:
        _k32.CloseHandle(snap)
    return None


def _read_u32(handle, address: int) -> int:
    buf = (ctypes.c_ubyte * 4)()
    read = ctypes.c_size_t()
    if not _k32.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, 4, ctypes.byref(read)):
        raise OSError(ctypes.get_last_error())
    return struct.unpack("<I", bytes(buf))[0]


def _read_string_ascii(handle, address: int, max_len: int = 64) -> str:
    buf = (ctypes.c_ubyte * max_len)()
    read = ctypes.c_size_t()
    if not _k32.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, max_len, ctypes.byref(read)):
        raise OSError(ctypes.get_last_error())
    return bytes(buf).split(b"\x00", 1)[0].decode("ascii", errors="ignore")


def _module_base(handle, module_name: str) -> Optional[int]:
    pid = int(_k32.GetProcessId(handle))
    snap = _k32.CreateToolhelp32Snapshot(_TH32CS_SNAPMODULE | _TH32CS_SNAPMODULE32, pid)
    if not snap or snap == _INVALID:
        return None
    entry = MODULEENTRY32W()
    entry.dwSize = ctypes.sizeof(MODULEENTRY32W)
    try:
        if not _k32.Module32FirstW(snap, ctypes.byref(entry)):
            return None
        target = module_name.lower()
        while True:
            if entry.szModule.lower() == target:
                return int(entry.modBaseAddr or 0) or None
            if not _k32.Module32NextW(snap, ctypes.byref(entry)):
                break
    finally:
        _k32.CloseHandle(snap)
    return None


def _pe_machine(dll_path: Path) -> Optional[int]:
    try:
        with dll_path.open("rb") as f:
            if f.read(2) != b"MZ":
                return None
            f.seek(0x3C)
            e_lfanew = struct.unpack("<I", f.read(4))[0]
            f.seek(e_lfanew)
            if f.read(4) != b"PE\x00\x00":
                return None
            return struct.unpack("<H", f.read(2))[0]
    except OSError:
        return None


def _target_export(handle, export_name: str) -> Optional[int]:
    base = _module_base(handle, "kernel32.dll")
    if not base:
        logger.warning("Could not find kernel32.dll in grandia.exe")
        return None
    try:
        e_lfanew = _read_u32(handle, base + 0x3C)
        export_rva = _read_u32(handle, base + e_lfanew + 0x18 + 0x60)
        exp = base + export_rva
        num_names = _read_u32(handle, exp + 0x18)
        addr_funcs = base + _read_u32(handle, exp + 0x1C)
        addr_names = base + _read_u32(handle, exp + 0x20)
        addr_ords = base + _read_u32(handle, exp + 0x24)
        for i in range(num_names):
            name_rva = _read_u32(handle, addr_names + i * 4)
            name = _read_string_ascii(handle, base + name_rva)
            if name == export_name:
                buf = (ctypes.c_ubyte * 2)()
                read = ctypes.c_size_t()
                _k32.ReadProcessMemory(
                    handle, ctypes.c_void_p(addr_ords + i * 2), buf, 2, ctypes.byref(read)
                )
                ordinal = struct.unpack("<H", bytes(buf))[0]
                func_rva = _read_u32(handle, addr_funcs + ordinal * 4)
                return base + func_rva
    except OSError as exc:
        logger.debug("resolve %s failed: %s", export_name, exc)
    return None


def _target_loadlibraryw(handle) -> Optional[int]:
    return _target_export(handle, "LoadLibraryW")


def _build_loadlibrary_stub(load_library_w: int, get_last_error: int) -> bytes:
    """x86 stdcall ThreadProc(path) → HMODULE on success, Win32 error (<64K) on failure."""
    return b"".join(
        [
            b"\x55",  # push ebp
            b"\x8B\xEC",  # mov ebp, esp
            b"\xFF\x75\x08",  # push [ebp+8]
            b"\xB8" + struct.pack("<I", load_library_w & 0xFFFFFFFF),
            b"\xFF\xD0",  # call eax
            b"\x85\xC0",  # test eax, eax
            b"\x75\x10",  # jnz done
            b"\xB8" + struct.pack("<I", get_last_error & 0xFFFFFFFF),
            b"\xFF\xD0",  # call eax
            b"\x85\xC0",  # test eax, eax
            b"\x75\x05",  # jnz done
            b"\xB8\xFF\xFF\xFF\xFF",  # mov eax, -1
            # done:
            b"\x5D",  # pop ebp
            b"\xC2\x04\x00",  # ret 4
        ]
    )


def _format_load_failure(code: int, dll_path: Path) -> str:
    hint = _WIN32_ERROR_HINTS.get(code, "See Win32 error code")
    return (
        f"LoadLibraryW failed for {dll_path} (remote error={code}: {hint}). "
        "Common fixes: allow the DLL in antivirus, use the latest Win32 apworld, "
        "run Archipelago and Steam as the same Windows user."
    )


def inject_dll(pid: int, dll_path: Path) -> bool:
    dll_path = Path(dll_path).resolve()
    if not dll_path.is_file():
        logger.error("DLL not found: %s", dll_path)
        return False

    machine = _pe_machine(dll_path)
    if machine == _IMAGE_FILE_MACHINE_AMD64:
        logger.error(
            "DLL is x64 (%s) — grandia.exe is Win32/x86. Rebuild/reinstall the Win32 apworld.",
            dll_path,
        )
        return False
    if machine is not None and machine != _IMAGE_FILE_MACHINE_I386:
        logger.error("DLL has unexpected PE machine 0x%04X (%s)", machine, dll_path)
        return False
    if machine is None:
        logger.warning("Could not parse PE headers for %s — injecting anyway", dll_path)

    handle = _k32.OpenProcess(_PROCESS_ALL, False, pid)
    if not handle:
        logger.error("OpenProcess failed (%s)", ctypes.get_last_error())
        return False

    remote_path = None
    remote_stub = None
    thread = None
    try:
        load_lib = _target_loadlibraryw(handle)
        get_last_error = _target_export(handle, "GetLastError")
        if not load_lib:
            logger.error("Could not resolve LoadLibraryW in grandia.exe")
            return False

        logger.info("Injecting %s into pid %s", dll_path, pid)

        path_bytes = str(dll_path).encode("utf-16-le") + b"\x00\x00"
        remote_path = _k32.VirtualAllocEx(
            handle, None, len(path_bytes), _MEM_COMMIT_RESERVE, _PAGE_READWRITE
        )
        if not remote_path:
            logger.error("VirtualAllocEx(path) failed (%s)", ctypes.get_last_error())
            return False

        written = ctypes.c_size_t()
        if not _k32.WriteProcessMemory(
            handle, remote_path, path_bytes, len(path_bytes), ctypes.byref(written)
        ):
            logger.error("WriteProcessMemory(path) failed (%s)", ctypes.get_last_error())
            return False

        start_addr: int = int(load_lib)
        if get_last_error:
            stub = _build_loadlibrary_stub(int(load_lib), int(get_last_error))
            remote_stub = _k32.VirtualAllocEx(
                handle, None, len(stub), _MEM_COMMIT_RESERVE, _PAGE_EXECUTE_READWRITE
            )
            if remote_stub and _k32.WriteProcessMemory(
                handle, remote_stub, stub, len(stub), ctypes.byref(written)
            ):
                start_addr = int(remote_stub)
            else:
                logger.warning(
                    "Could not plant LoadLibrary stub (%s) — falling back without remote GetLastError",
                    ctypes.get_last_error(),
                )
                if remote_stub:
                    _k32.VirtualFreeEx(handle, remote_stub, 0, 0x8000)
                    remote_stub = None

        thread = _k32.CreateRemoteThread(
            handle, None, 0, ctypes.c_void_p(start_addr), remote_path, 0, None
        )
        if not thread:
            logger.error("CreateRemoteThread failed (%s)", ctypes.get_last_error())
            return False

        wait = _k32.WaitForSingleObject(thread, 60000)
        if wait != 0:
            logger.error("Remote LoadLibraryW timed out (WaitForSingleObject=%s)", wait)
            return False

        exit_code = wintypes.DWORD()
        _k32.GetExitCodeThread(thread, ctypes.byref(exit_code))
        value = int(exit_code.value)

        # Stub returns Win32 errors as small codes; bare LoadLibrary returns NULL (0).
        if value == 0 or value < 0x10000 or value == 0xFFFFFFFF:
            logger.error(_format_load_failure(value if value else 0, dll_path))
            return False

        logger.info("Injected %s (module=0x%X)", dll_path.name, value)
        return True
    finally:
        if thread:
            _k32.CloseHandle(thread)
        if remote_stub:
            _k32.VirtualFreeEx(handle, remote_stub, 0, 0x8000)
        if remote_path:
            _k32.VirtualFreeEx(handle, remote_path, 0, 0x8000)
        _k32.CloseHandle(handle)
