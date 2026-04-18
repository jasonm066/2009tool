// ============================================================
//  Minimal DLL injector for RobloxApp_client.exe
//  Uses CreateRemoteThread + LoadLibraryA (classic).
//
//  Usage: injector.exe [path\to\executor.dll]
//         default: .\executor.dll
// ============================================================

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <string>

static DWORD GetPidByName(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int main(int argc, char** argv) {
    // Resolve DLL path (absolute, so LoadLibrary works in the target's CWD)
    char dllPath[MAX_PATH];
    if (argc >= 2) {
        if (!GetFullPathNameA(argv[1], MAX_PATH, dllPath, nullptr)) {
            printf("[-] invalid DLL path: %s\n", argv[1]);
            return 1;
        }
    } else {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string p(exe);
        size_t slash = p.find_last_of("\\/");
        std::string base = (slash == std::string::npos) ? "" : p.substr(0, slash + 1);
        std::string full = base + "internal.dll";
        strncpy_s(dllPath, full.c_str(), MAX_PATH);
    }

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        printf("[-] DLL not found: %s\n", dllPath);
        return 1;
    }

    printf("[*] injecting: %s\n", dllPath);
    printf("[*] waiting for RobloxApp_client.exe...\n");
    DWORD pid = 0;
    while (!(pid = GetPidByName(L"RobloxApp_client.exe"))) Sleep(500);
    printf("[+] PID: %lu\n", pid);

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) { printf("[-] OpenProcess failed (%lu)\n", GetLastError()); return 1; }

    SIZE_T pathLen = strlen(dllPath) + 1;
    LPVOID remoteBuf = VirtualAllocEx(hProc, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE);
    if (!remoteBuf) {
        printf("[-] VirtualAllocEx failed (%lu)\n", GetLastError());
        CloseHandle(hProc); return 1;
    }
    if (!WriteProcessMemory(hProc, remoteBuf, dllPath, pathLen, nullptr)) {
        printf("[-] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc); return 1;
    }

    // LoadLibraryA lives in kernel32 at the same address in every 32-bit process on the same OS
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");

    HANDLE remoteThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, remoteBuf,
                                             0, nullptr);
    if (!remoteThread) {
        printf("[-] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc); return 1;
    }

    WaitForSingleObject(remoteThread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(remoteThread, &exitCode);
    printf("[+] remote LoadLibrary returned: 0x%08lX %s\n",
           exitCode, exitCode ? "(OK)" : "(FAILED — DLL returned FALSE)");

    CloseHandle(remoteThread);
    VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return exitCode ? 0 : 1;
}
