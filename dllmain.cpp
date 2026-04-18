// ============================================================
//  Internal All-in-One: ESP + Lua executor for 2009 Roblox.
//  Inject this DLL into RobloxApp_client.exe via injector.exe.
//  Press INSERT to toggle the menu.
// ============================================================

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include "config.h"
#include "lua_api.h"
#include "executor.h"
#include "hooks.h"

Config  g_cfg;
LuaAPI  g_lua;

static void OpenDebugConsole() {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    SetConsoleTitleA("Internal Debug");
}

static void DLog(const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    printf("[init] %s\n", buf); fflush(stdout);
    OutputDebugStringA("[init] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
}

extern "C" void DLog_external(const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    printf("[hook] %s\n", buf); fflush(stdout);
    OutputDebugStringA("[hook] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
}

static DWORD WINAPI InitThread(LPVOID) {
    OpenDebugConsole();
    DLog("InitThread started");

    // Wait for d3d9.dll AND for the game to have rendered at least once.
    // We approximate "rendered" by waiting a long grace period.
    while (!GetModuleHandleA("d3d9.dll")) { DLog("waiting for d3d9.dll"); Sleep(200); }
    DLog("d3d9.dll loaded; sleeping 3s for game to settle");
    Sleep(3000);

    DLog("resolving Lua API...");
    if (!g_lua.Init()) {
        DLog("FATAL: g_lua.Init failed");
        return 1;
    }
    DLog("Lua API OK (base=%p)", g_lua.base);
    executor::Init(&g_lua);

    if (g_cfg.Load()) DLog("config loaded");

    DLog("installing D3D9 hooks...");
    bool ok = false;
    __try {
        ok = hooks::Install();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DLog("FATAL: hooks::Install threw 0x%08X", GetExceptionCode());
        return 1;
    }
    if (!ok) {
        DLog("FATAL: hooks::Install returned false");
        return 1;
    }

    DLog("internal ready - press INSERT for menu");
    executor::Log("internal ready - press INSERT for menu");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        hooks::Uninstall();
    }
    return TRUE;
}
