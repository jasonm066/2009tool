#include "hooks.h"
#include "esp.h"
#include "menu.h"
#include "executor.h"
#include "config.h"

#include <Windows.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <cstdio>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_impl_dx9.h"
#include "vendor/imgui/imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")

extern Config g_cfg;
extern "C" void DLog_external(const char* fmt, ...);
#define HLOG(...) DLog_external(__VA_ARGS__)

namespace hooks {

static int InstrLen(const uint8_t* p) {
    uint8_t op = p[0];
    if (op >= 0x50 && op <= 0x5F) return 1; // push/pop r32
    if (op == 0x90) return 1;                // nop
    if (op == 0xC3) return 1;                // ret
    if (op == 0x6A) return 2;                // push imm8
    if (op == 0xEB) return 2;                // jmp short
    if (op >= 0xB8 && op <= 0xBF) return 5; // mov r32, imm32
    if (op == 0xE8 || op == 0xE9) return 5; // call/jmp rel32
    if (op == 0x68) return 5;                // push imm32
    if (op == 0x8B && (p[1] & 0xC0) == 0xC0) return 2; // mov reg, reg
    if (op == 0x89 && (p[1] & 0xC0) == 0xC0) return 2; // mov reg, reg
    if (op == 0x33 && (p[1] & 0xC0) == 0xC0) return 2; // xor reg, reg
    if (op == 0x83 && (p[1] & 0xF8) == 0xE8) return 3; // sub r/m32, imm8
    if (op == 0x83 && (p[1] & 0xF8) == 0xC0) return 3; // add r/m32, imm8
    if (op == 0x81 && (p[1] & 0xF8) == 0xE8) return 6; // sub r/m32, imm32
    if (op == 0x81 && (p[1] & 0xF8) == 0xC0) return 6; // add r/m32, imm32
    if (op == 0x8B && p[1] == 0xFF) return 2;           // mov edi, edi (hot-patch)
    if (op == 0xFF) {
        uint8_t mod = p[1] >> 6, rm = p[1] & 7;
        if (mod == 0 && rm == 5) return 6; // jmp/call [disp32]
        if (mod == 3) return 2;            // jmp/call reg
    }
    return 0;
}

static void* FollowJmpStub(void* target) {
    const uint8_t* p = (const uint8_t*)target;
    if (p[0] == 0xFF && p[1] == 0x25) {
        // x86: disp32 is absolute, not RIP-relative
        uint32_t addrOfPtr = *(const uint32_t*)(p + 2);
        return *(void**)(uintptr_t)addrOfPtr;
    }
    if (p[0] == 0xE9) {
        int32_t rel = *(const int32_t*)(p + 1);
        return (void*)((uintptr_t)target + 5 + rel);
    }
    return target;
}

struct Detour {
    void*    target       = nullptr;
    uint8_t  origBytes[16]= {};
    int      origLen      = 0;
    uint8_t* trampoline   = nullptr;
    bool     installed    = false;
};

static bool InstallInlineHook(Detour& d, void* target, void* detour) {
    d.target = target;
    const uint8_t* p = (const uint8_t*)target;
    int total = 0;
    while (total < 5) {
        int n = InstrLen(p + total);
        if (n == 0) { HLOG("    InstrLen failed at offset %d (byte 0x%02X)", total, p[total]); return false; }
        total += n;
        if (total > 15) { HLOG("    prologue too long"); return false; }
    }
    d.origLen = total;
    HLOG("    prologue %d bytes", total);

    d.trampoline = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!d.trampoline) return false;

    memcpy(d.origBytes,  target, total);
    memcpy(d.trampoline, target, total);
    d.trampoline[total] = 0xE9;
    int32_t relBack = (int32_t)((uintptr_t)target + total - ((uintptr_t)d.trampoline + total + 5));
    memcpy(d.trampoline + total + 1, &relBack, 4);

    DWORD oldProt;
    if (!VirtualProtect(target, total, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    uint8_t patch[16];
    memset(patch, 0x90, total); // NOP-pad bytes beyond the 5-byte JMP
    patch[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)detour - ((uintptr_t)target + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy(target, patch, total);
    VirtualProtect(target, total, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, total);

    d.installed = true;
    return true;
}

static void UninstallInlineHook(Detour& d) {
    if (!d.installed) return;
    DWORD oldProt;
    if (VirtualProtect(d.target, d.origLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(d.target, d.origBytes, d.origLen);
        VirtualProtect(d.target, d.origLen, oldProt, &oldProt);
    }
    if (d.trampoline) VirtualFree(d.trampoline, 0, MEM_RELEASE);
    d.installed = false;
}

constexpr int VTBL_RESET    = 16;
constexpr int VTBL_ENDSCENE = 42;

using PFN_EndScene = HRESULT (WINAPI*)(IDirect3DDevice9*);
using PFN_Reset    = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static Detour       d_endScene{}, d_reset{};
static PFN_EndScene origEndScene = nullptr;
static PFN_Reset    origReset    = nullptr;
static void**       g_deviceVtbl = nullptr;

// DirectInput8 vtable patch. 2009 Roblox polls the keyboard/mouse via
// IDirectInputDevice8::GetDeviceState / GetDeviceData (slots 9 and 10).
// dinput8.dll hands out a shared vtable per interface variant (A vs W),
// so patching those two slots on a throwaway device silences input for
// every device the game creates from the same factory.
using PFN_GetDeviceState = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using PFN_GetDeviceData  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

struct DiPatch {
    void**             vtbl    = nullptr;
    PFN_GetDeviceState origGds = nullptr;
    PFN_GetDeviceData  origGdd = nullptr;
};
static DiPatch g_diA{}, g_diW{};

// Roblox 2009 (G3D engine) sets the class cursor once via SetClassLongA and never
// calls SetCursor per-frame, so hooking SetCursor alone doesn't hide it. We also
// clear the class cursor on both the render HWND and its top-level parent while
// the menu is open, restoring on close.
using PFN_SetCursorPos = BOOL    (WINAPI*)(int, int);
using PFN_ClipCursor   = BOOL    (WINAPI*)(const RECT*);
using PFN_SetCursor    = HCURSOR (WINAPI*)(HCURSOR);
static Detour          d_setCursorPos{}, d_clipCursor{}, d_setCursor{};
static PFN_SetCursorPos origSetCursorPos = nullptr;
static PFN_ClipCursor   origClipCursor   = nullptr;
static PFN_SetCursor    origSetCursor    = nullptr;

static volatile LONG g_scpCalls = 0, g_ccCalls = 0, g_scCalls = 0;

static HRESULT STDMETHODCALLTYPE HookedGetDeviceStateA(IDirectInputDevice8W* dev, DWORD cb, LPVOID data) {
    if (g_cfg.showMenu) {
        if (data && cb) memset(data, 0, cb);
        return DI_OK;
    }
    return g_diA.origGds(dev, cb, data);
}
static HRESULT STDMETHODCALLTYPE HookedGetDeviceDataA(IDirectInputDevice8W* dev, DWORD cb,
                                                     LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    if (g_cfg.showMenu) { if (inOut) *inOut = 0; return DI_OK; }
    return g_diA.origGdd(dev, cb, data, inOut, flags);
}
static HRESULT STDMETHODCALLTYPE HookedGetDeviceStateW(IDirectInputDevice8W* dev, DWORD cb, LPVOID data) {
    if (g_cfg.showMenu) {
        if (data && cb) memset(data, 0, cb);
        return DI_OK;
    }
    return g_diW.origGds(dev, cb, data);
}
static HRESULT STDMETHODCALLTYPE HookedGetDeviceDataW(IDirectInputDevice8W* dev, DWORD cb,
                                                     LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    if (g_cfg.showMenu) { if (inOut) *inOut = 0; return DI_OK; }
    return g_diW.origGdd(dev, cb, data, inOut, flags);
}

static BOOL WINAPI HookedSetCursorPos(int x, int y) {
    InterlockedIncrement(&g_scpCalls);
    if (g_cfg.showMenu) return TRUE;
    return origSetCursorPos(x, y);
}
static BOOL WINAPI HookedClipCursor(const RECT* r) {
    InterlockedIncrement(&g_ccCalls);
    if (g_cfg.showMenu) return TRUE;
    return origClipCursor(r);
}
static HCURSOR WINAPI HookedSetCursor(HCURSOR h) {
    InterlockedIncrement(&g_scCalls);
    if (g_cfg.showMenu) return origSetCursor(nullptr);
    return origSetCursor(h);
}

static bool imguiReady = false;
static HWND gameHwnd   = nullptr;

static void InitImGuiOnce(IDirect3DDevice9* dev) {
    if (imguiReady) return;
    D3DDEVICE_CREATION_PARAMETERS p{};
    if (FAILED(dev->GetCreationParameters(&p))) return;
    gameHwnd = p.hFocusWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(gameHwnd);
    ImGui_ImplDX9_Init(dev);
    HLOG("  ImGui initialized on hwnd=%p", gameHwnd);
    imguiReady = true;
}

static ImGuiKey VkToImGuiKey(int vk) {
    switch (vk) {
        case VK_TAB:        return ImGuiKey_Tab;
        case VK_LEFT:       return ImGuiKey_LeftArrow;
        case VK_RIGHT:      return ImGuiKey_RightArrow;
        case VK_UP:         return ImGuiKey_UpArrow;
        case VK_DOWN:       return ImGuiKey_DownArrow;
        case VK_PRIOR:      return ImGuiKey_PageUp;
        case VK_NEXT:       return ImGuiKey_PageDown;
        case VK_HOME:       return ImGuiKey_Home;
        case VK_END:        return ImGuiKey_End;
        case VK_INSERT:     return ImGuiKey_Insert;
        case VK_DELETE:     return ImGuiKey_Delete;
        case VK_BACK:       return ImGuiKey_Backspace;
        case VK_RETURN:     return ImGuiKey_Enter;
        case VK_ESCAPE:     return ImGuiKey_Escape;
        case VK_SPACE:      return ImGuiKey_Space;
        case VK_LSHIFT:     return ImGuiKey_LeftShift;
        case VK_RSHIFT:     return ImGuiKey_RightShift;
        case VK_LCONTROL:   return ImGuiKey_LeftCtrl;
        case VK_RCONTROL:   return ImGuiKey_RightCtrl;
        case VK_LMENU:      return ImGuiKey_LeftAlt;
        case VK_RMENU:      return ImGuiKey_RightAlt;
        case VK_OEM_COMMA:  return ImGuiKey_Comma;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_MINUS:  return ImGuiKey_Minus;
        case VK_OEM_PLUS:   return ImGuiKey_Equal;
        case VK_OEM_1:      return ImGuiKey_Semicolon;
        case VK_OEM_2:      return ImGuiKey_Slash;
        case VK_OEM_3:      return ImGuiKey_GraveAccent;
        case VK_OEM_4:      return ImGuiKey_LeftBracket;
        case VK_OEM_5:      return ImGuiKey_Backslash;
        case VK_OEM_6:      return ImGuiKey_RightBracket;
        case VK_OEM_7:      return ImGuiKey_Apostrophe;
        default: break;
    }
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    return ImGuiKey_None;
}

static void FeedImGuiInput() {
    ImGuiIO& io = ImGui::GetIO();

    POINT cp;
    if (GetCursorPos(&cp) && gameHwnd) {
        ScreenToClient(gameHwnd, &cp);
        io.AddMousePosEvent((float)cp.x, (float)cp.y);
    }

    static bool prevBtn[5] = {};
    static const int btnVks[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 5; ++i) {
        bool down = (GetAsyncKeyState(btnVks[i]) & 0x8000) != 0;
        if (down != prevBtn[i]) { io.AddMouseButtonEvent(i, down); prevBtn[i] = down; }
    }

    // Build kbdState from GetAsyncKeyState — GetKeyboardState is thread-local and
    // returns 0 for Shift/Ctrl/CapsLock on our render thread.
    BYTE kbdState[256] = {};
    HKL hkl = GetKeyboardLayout(0);
    for (int v = 1; v < 256; ++v)
        if ((GetAsyncKeyState(v) & 0x8000) != 0) kbdState[v] = 0x80;

    // Track lock-key toggles manually since GetAsyncKeyState has no toggle bit.
    static bool capsToggle   = (GetKeyState(VK_CAPITAL) & 1) != 0;
    static bool numToggle    = (GetKeyState(VK_NUMLOCK) & 1) != 0;
    static bool scrollToggle = (GetKeyState(VK_SCROLL)  & 1) != 0;
    static bool prevCaps = false, prevNum = false, prevScroll = false;
    bool capsDown   = (kbdState[VK_CAPITAL] & 0x80) != 0;
    bool numDown    = (kbdState[VK_NUMLOCK] & 0x80) != 0;
    bool scrollDown = (kbdState[VK_SCROLL]  & 0x80) != 0;
    if (capsDown   && !prevCaps)   capsToggle   = !capsToggle;
    if (numDown    && !prevNum)    numToggle    = !numToggle;
    if (scrollDown && !prevScroll) scrollToggle = !scrollToggle;
    prevCaps = capsDown; prevNum = numDown; prevScroll = scrollDown;
    if (capsToggle)   kbdState[VK_CAPITAL] |= 1;
    if (numToggle)    kbdState[VK_NUMLOCK] |= 1;
    if (scrollToggle) kbdState[VK_SCROLL]  |= 1;

    static bool prevKey[256] = {};
    for (int vk = 0x08; vk < 0xFF; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;

        bool down = (kbdState[vk] & 0x80) != 0;
        if (down == prevKey[vk]) continue;
        prevKey[vk] = down;

        ImGuiKey ik = VkToImGuiKey(vk);
        if (ik != ImGuiKey_None) io.AddKeyEvent(ik, down);

        if (vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT)
            io.AddKeyEvent(ImGuiMod_Shift, (kbdState[VK_SHIFT]   & 0x80) != 0);
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
            io.AddKeyEvent(ImGuiMod_Ctrl,  (kbdState[VK_CONTROL] & 0x80) != 0);
        if (vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU)
            io.AddKeyEvent(ImGuiMod_Alt,   (kbdState[VK_MENU]    & 0x80) != 0);

        if (down) {
            UINT scan = MapVirtualKeyExA(vk, MAPVK_VK_TO_VSC, hkl);
            wchar_t buf[8] = {};
            int n = ToUnicodeEx((UINT)vk, scan, kbdState, buf, (int)(sizeof(buf)/sizeof(buf[0])), 0, hkl);
            if (n > 0) {
                for (int i = 0; i < n; ++i)
                    if (buf[i] >= 0x20 || buf[i] == '\t')
                        io.AddInputCharacterUTF16((unsigned short)buf[i]);
            }
            if (n < 0) {
                // ToUnicodeEx set dead-key state; flush it so subsequent keys aren't corrupted.
                wchar_t junk[4];
                ToUnicodeEx(VK_SPACE, MapVirtualKeyExA(VK_SPACE, MAPVK_VK_TO_VSC, hkl),
                            kbdState, junk, 4, 0, hkl);
            }
        }
    }
}

static void HandleToggleKey() {
    static bool prevDown = false;
    bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (down && !prevDown) g_cfg.showMenu = !g_cfg.showMenu;
    prevDown = down;
}

static int     osCursorHideDelta = 0;
static HWND    scrubbedTopHwnd   = nullptr;
static HCURSOR savedTopCursor    = nullptr;
static HWND    scrubbedChildHwnd = nullptr;
static HCURSOR savedChildCursor  = nullptr;
static bool    loggedClassInfo   = false;

static HWND GetTopLevelGameWindow() {
    if (!gameHwnd) return nullptr;
    HWND h = gameHwnd;
    while (HWND parent = GetAncestor(h, GA_PARENT)) {
        if (parent == GetDesktopWindow()) break;
        h = parent;
    }
    return h;
}

static void UpdateOsCursorVisibility(bool menuOpen) {
    ImGuiIO& io = ImGui::GetIO();

    if (!loggedClassInfo && gameHwnd) {
        HWND top = GetTopLevelGameWindow();
        char cls[128] = {};
        GetClassNameA(gameHwnd, cls, sizeof(cls));
        HLOG("  gameHwnd=%p class=%s classCursor=%p", gameHwnd, cls, (void*)GetClassLongPtrA(gameHwnd, -12));
        if (top && top != gameHwnd) {
            GetClassNameA(top, cls, sizeof(cls));
            HLOG("  topHwnd=%p class=%s classCursor=%p", top, cls, (void*)GetClassLongPtrA(top, -12));
        }
        loggedClassInfo = true;
    }

    if (menuOpen) {
        io.MouseDrawCursor = true;
        HWND top = GetTopLevelGameWindow();
        if (!scrubbedChildHwnd && gameHwnd) {
            scrubbedChildHwnd = gameHwnd;
            savedChildCursor  = (HCURSOR)SetClassLongPtrA(gameHwnd, -12, 0);
        }
        if (!scrubbedTopHwnd && top && top != gameHwnd) {
            scrubbedTopHwnd = top;
            savedTopCursor  = (HCURSOR)SetClassLongPtrA(top, -12, 0);
        }
        SetCursor(nullptr);
        if (osCursorHideDelta == 0) {
            int c = ShowCursor(FALSE);
            osCursorHideDelta = 1;
            while (c >= 0) { c = ShowCursor(FALSE); osCursorHideDelta++; }
        }
    } else {
        io.MouseDrawCursor = false;
        if (scrubbedChildHwnd) {
            SetClassLongPtrA(scrubbedChildHwnd, -12, (LONG_PTR)savedChildCursor);
            scrubbedChildHwnd = nullptr; savedChildCursor = nullptr;
        }
        if (scrubbedTopHwnd) {
            SetClassLongPtrA(scrubbedTopHwnd, -12, (LONG_PTR)savedTopCursor);
            scrubbedTopHwnd = nullptr; savedTopCursor = nullptr;
        }
        while (osCursorHideDelta > 0) { ShowCursor(TRUE); osCursorHideDelta--; }
    }
}

static int s_cachedSw = 0, s_cachedSh = 0;

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* dev) {
    InitImGuiOnce(dev);
    executor::Tick();

    // Backbuffer size only changes across Reset (which we hook); no need to
    // walk the COM chain every frame.
    if (!s_cachedSw || !s_cachedSh) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            D3DSURFACE_DESC desc; bb->GetDesc(&desc);
            s_cachedSw = (int)desc.Width; s_cachedSh = (int)desc.Height;
            bb->Release();
        }
    }
    int sw = s_cachedSw, sh = s_cachedSh;

    HandleToggleKey();
    UpdateOsCursorVisibility(g_cfg.showMenu);

    static DWORD lastLog = 0;
    DWORD now = GetTickCount();
    if (now - lastLog > 5000) {
        lastLog = now;
        HLOG("  hook fires: SetCursorPos=%ld ClipCursor=%ld SetCursor=%ld", g_scpCalls, g_ccCalls, g_scCalls);
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (g_cfg.showMenu) FeedImGuiInput(); // must feed before NewFrame consumes io state
    ImGui::NewFrame();

    if (sw > 0 && sh > 0) esp::Draw(sw, sh, g_cfg);
    menu::Draw(g_cfg);

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    return origEndScene(dev);
}

static HRESULT WINAPI HookedReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
    s_cachedSw = s_cachedSh = 0; // force re-query on next EndScene
    HRESULT hr = origReset(dev, pp);
    if (imguiReady && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

static void** GetD3D9DeviceVTable() {
    HLOG("  Direct3DCreate9...");
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { HLOG("  Direct3DCreate9 returned null"); return nullptr; }

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) hwnd = GetDesktopWindow();

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow    = hwnd;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                   &pp, &dev);
    HLOG("  CreateDevice HAL: hr=0x%08X dev=%p", hr, dev);
    if (FAILED(hr) || !dev) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        HLOG("  CreateDevice NULLREF: hr=0x%08X dev=%p", hr, dev);
    }
    if (FAILED(hr) || !dev) { d3d->Release(); return nullptr; }

    void** vtbl = *reinterpret_cast<void***>(dev);
    HLOG("  vtbl=%p  EndScene=%p  Reset=%p", vtbl, vtbl[VTBL_ENDSCENE], vtbl[VTBL_RESET]);

    uint8_t* es = (uint8_t*)vtbl[VTBL_ENDSCENE];
    HLOG("  EndScene prologue: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
         es[0], es[1], es[2], es[3], es[4], es[5], es[6], es[7], es[8], es[9]);

    // Intentionally leaking dev/d3d: releasing a HAL device created on a borrowed HWND
    // with DISABLE_DRIVER_MANAGEMENT can crash the graphics driver on some setups.
    // The vtable pointers live in d3d9.dll and remain valid regardless.
    return vtbl;
}

bool Install() {
    HLOG("hooks::Install entry");
    g_deviceVtbl = GetD3D9DeviceVTable();
    if (!g_deviceVtbl) { HLOG("  no vtable"); return false; }

    origEndScene = (PFN_EndScene)g_deviceVtbl[VTBL_ENDSCENE];
    origReset    = (PFN_Reset)   g_deviceVtbl[VTBL_RESET];

    if (!InstallInlineHook(d_endScene, (void*)origEndScene, (void*)&HookedEndScene)) {
        HLOG("  EndScene hook failed"); return false;
    }
    origEndScene = (PFN_EndScene)d_endScene.trampoline;

    if (!InstallInlineHook(d_reset, (void*)origReset, (void*)&HookedReset)) {
        HLOG("  Reset hook failed"); return false;
    }
    origReset = (PFN_Reset)d_reset.trampoline;

    // Patch IDirectInputDevice8 vtable slots 9/10 (GetDeviceState/GetDeviceData) for both
    // ANSI and Unicode interfaces so every keyboard/mouse device Roblox creates goes silent
    // while the menu is open.
    HMODULE di8 = LoadLibraryA("dinput8.dll");
    if (di8) {
        using PFN_DI8Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        auto di8Create = (PFN_DI8Create)GetProcAddress(di8, "DirectInput8Create");
        HINSTANCE hInst = GetModuleHandleA(nullptr);

        auto patchA = [&]() {
            IDirectInput8A* dip = nullptr;
            HRESULT hr = di8Create(hInst, DIRECTINPUT_VERSION, IID_IDirectInput8A, (LPVOID*)&dip, nullptr);
            if (FAILED(hr) || !dip) { HLOG("  DI8Create(A) failed hr=0x%08X", hr); return; }
            IDirectInputDevice8A* dev = nullptr;
            hr = dip->CreateDevice(GUID_SysKeyboard, &dev, nullptr);
            if (FAILED(hr) || !dev) { HLOG("  CreateDevice(A) failed hr=0x%08X", hr); dip->Release(); return; }

            void** vtbl = *(void***)dev;
            g_diA.vtbl    = vtbl;
            g_diA.origGds = (PFN_GetDeviceState)vtbl[9];
            g_diA.origGdd = (PFN_GetDeviceData) vtbl[10];

            DWORD oldProt;
            if (VirtualProtect(&vtbl[9], sizeof(void*) * 2, PAGE_READWRITE, &oldProt)) {
                vtbl[9]  = (void*)&HookedGetDeviceStateA;
                vtbl[10] = (void*)&HookedGetDeviceDataA;
                VirtualProtect(&vtbl[9], sizeof(void*) * 2, oldProt, &oldProt);
                HLOG("  DI vtable(A) patched @%p", vtbl);
            } else { g_diA.vtbl = nullptr; HLOG("  DI VirtualProtect(A) failed"); }

            dev->Release(); dip->Release();
        };
        auto patchW = [&]() {
            IDirectInput8W* dip = nullptr;
            HRESULT hr = di8Create(hInst, DIRECTINPUT_VERSION, IID_IDirectInput8W, (LPVOID*)&dip, nullptr);
            if (FAILED(hr) || !dip) { HLOG("  DI8Create(W) failed hr=0x%08X", hr); return; }
            IDirectInputDevice8W* dev = nullptr;
            hr = dip->CreateDevice(GUID_SysKeyboard, &dev, nullptr);
            if (FAILED(hr) || !dev) { HLOG("  CreateDevice(W) failed hr=0x%08X", hr); dip->Release(); return; }

            void** vtbl = *(void***)dev;
            g_diW.vtbl    = vtbl;
            g_diW.origGds = (PFN_GetDeviceState)vtbl[9];
            g_diW.origGdd = (PFN_GetDeviceData) vtbl[10];

            DWORD oldProt;
            if (VirtualProtect(&vtbl[9], sizeof(void*) * 2, PAGE_READWRITE, &oldProt)) {
                vtbl[9]  = (void*)&HookedGetDeviceStateW;
                vtbl[10] = (void*)&HookedGetDeviceDataW;
                VirtualProtect(&vtbl[9], sizeof(void*) * 2, oldProt, &oldProt);
                HLOG("  DI vtable(W) patched @%p", vtbl);
            } else { g_diW.vtbl = nullptr; HLOG("  DI VirtualProtect(W) failed"); }

            dev->Release(); dip->Release();
        };
        patchA();
        patchW();
    } else {
        HLOG("  dinput8.dll not loaded");
    }

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        struct Spec { const char* name; void* detour; Detour* det; void** pOrig; };
        Spec specs[] = {
            { "SetCursorPos", (void*)&HookedSetCursorPos, &d_setCursorPos, (void**)&origSetCursorPos },
            { "ClipCursor",   (void*)&HookedClipCursor,   &d_clipCursor,   (void**)&origClipCursor   },
            { "SetCursor",    (void*)&HookedSetCursor,    &d_setCursor,    (void**)&origSetCursor    },
        };
        for (auto& s : specs) {
            void* p = (void*)GetProcAddress(user32, s.name);
            if (!p) continue;
            void* real = p;
            for (int i = 0; i < 4 && real; ++i) {
                void* next = FollowJmpStub(real);
                if (next == real) break;
                real = next;
            }
            if (real && InstallInlineHook(*s.det, real, s.detour)) {
                *s.pOrig = s.det->trampoline;
                HLOG("  %s hooked @%p", s.name, real);
            } else {
                HLOG("  %s hook FAILED", s.name);
            }
        }
    }

    HLOG("  hooks installed");
    return true;
}

void Uninstall() {
    UninstallInlineHook(d_endScene);
    UninstallInlineHook(d_reset);
    UninstallInlineHook(d_setCursorPos);
    UninstallInlineHook(d_clipCursor);
    UninstallInlineHook(d_setCursor);

    auto restoreDi = [](DiPatch& slot) {
        if (!slot.vtbl) return;
        DWORD oldProt;
        if (VirtualProtect(&slot.vtbl[9], sizeof(void*) * 2, PAGE_READWRITE, &oldProt)) {
            slot.vtbl[9]  = (void*)slot.origGds;
            slot.vtbl[10] = (void*)slot.origGdd;
            VirtualProtect(&slot.vtbl[9], sizeof(void*) * 2, oldProt, &oldProt);
        }
        slot = {};
    };
    restoreDi(g_diA);
    restoreDi(g_diW);
    if (imguiReady) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady = false;
    }
}

} // namespace hooks
