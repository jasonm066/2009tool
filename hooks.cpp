// ============================================================
//  hooks.cpp
//
//  Installs D3D9 EndScene + Reset inline hooks so we can draw ImGui
//  over the game's own backbuffer, and cursor-related user32 hooks
//  so the menu cursor works cleanly.
//
//  The hook entry (`HookedEndScene`) also tick-drains the executor's
//  pending-script queue, so Lua runs on the real game thread with no
//  thread-safety gymnastics. Input is fed from polled state rather
//  than a WndProc subclass - see FeedImGuiInput().
// ============================================================

#include "hooks.h"
#include "esp.h"
#include "menu.h"
#include "executor.h"
#include "config.h"

#include <Windows.h>
#include <d3d9.h>
#include <cstdio>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_impl_dx9.h"
#include "vendor/imgui/imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")

extern Config g_cfg;  // defined in dllmain.cpp

// Forward decl from dllmain.cpp
extern "C" void DLog_external(const char* fmt, ...);
#define HLOG(...) DLog_external(__VA_ARGS__)

namespace hooks {

// ----- Inline detour with minimal x86 instruction-length decoder -----
// We need to copy at least 5 complete instructions' worth of bytes to the
// trampoline, then NOP-pad and JMP back to (target + bytesCopied).
//
// The decoder only handles instruction forms commonly found in d3d9 function
// prologues; if it sees something unknown it bails out.
static int InstrLen(const uint8_t* p) {
    uint8_t op = p[0];
    // 1-byte no-operand: push reg, pop reg, ret variants, etc.
    if ((op >= 0x50 && op <= 0x5F)) return 1;        // push/pop r32
    if (op == 0x90) return 1;                         // nop
    if (op == 0xC3) return 1;                         // ret
    // 2-byte: short jumps, push imm8
    if (op == 0x6A) return 2;                         // push imm8
    if (op == 0xEB) return 2;                         // jmp short
    // 5-byte: mov reg, imm32 / call rel32 / jmp rel32 / push imm32
    if (op >= 0xB8 && op <= 0xBF) return 5;           // mov r32, imm32
    if (op == 0xE8 || op == 0xE9) return 5;           // call/jmp rel32
    if (op == 0x68) return 5;                         // push imm32
    // 2-byte: simple modrm forms used in prologues
    if (op == 0x8B && (p[1] & 0xC0) == 0xC0) return 2; // mov reg, reg
    if (op == 0x89 && (p[1] & 0xC0) == 0xC0) return 2; // mov reg, reg (alt)
    if (op == 0x33 && (p[1] & 0xC0) == 0xC0) return 2; // xor reg, reg
    // 3-byte: sub esp, imm8 etc
    if (op == 0x83 && (p[1] & 0xF8) == 0xE8) return 3; // sub r/m32, imm8 (modrm=11 101 reg)
    if (op == 0x83 && (p[1] & 0xF8) == 0xC0) return 3; // add r/m32, imm8
    // 6-byte: sub esp, imm32
    if (op == 0x81 && (p[1] & 0xF8) == 0xE8) return 6;
    if (op == 0x81 && (p[1] & 0xF8) == 0xC0) return 6;
    // mov edi, edi (hot-patch prologue)
    if (op == 0x8B && p[1] == 0xFF) return 2;
    // FF /r forms: jmp/call r/m32 (we only need the common absolute-indirect)
    if (op == 0xFF) {
        uint8_t modrm = p[1];
        uint8_t mod = modrm >> 6;
        uint8_t rm  = modrm & 7;
        // FF 25 disp32  -> jmp [disp32]       (API forwarder)
        // FF 15 disp32  -> call [disp32]
        if (mod == 0 && rm == 5) return 6;
        // FF /r reg-direct (jmp/call reg)
        if (mod == 3) return 2;
    }
    return 0; // unknown
}

// If the target is an `FF 25 disp32` forwarder, return the real destination.
// Otherwise return target unchanged.
static void* FollowJmpStub(void* target) {
    const uint8_t* p = (const uint8_t*)target;
    if (p[0] == 0xFF && p[1] == 0x25) {
        // x86: disp32 is an absolute address (not RIP-relative)
        uint32_t addrOfPtr = *(const uint32_t*)(p + 2);
        void* real = *(void**)(uintptr_t)addrOfPtr;
        return real;
    }
    // E9 rel32 relative jump
    if (p[0] == 0xE9) {
        int32_t rel = *(const int32_t*)(p + 1);
        return (void*)((uintptr_t)target + 5 + rel);
    }
    return target;
}

struct Detour {
    void*    target      = nullptr;
    uint8_t  origBytes[16]= {};
    int      origLen     = 0;
    uint8_t* trampoline  = nullptr;
    bool     installed   = false;
};

static bool InstallInlineHook(Detour& d, void* target, void* detour) {
    d.target = target;

    // Find the minimum number of complete instructions that cover >=5 bytes.
    const uint8_t* p = (const uint8_t*)target;
    int total = 0;
    while (total < 5) {
        int n = InstrLen(p + total);
        if (n == 0) {
            HLOG("    InstrLen failed at offset %d (byte 0x%02X)", total, p[total]);
            return false;
        }
        total += n;
        if (total > 15) { HLOG("    prologue too long"); return false; }
    }
    d.origLen = total;
    HLOG("    prologue %d bytes", total);

    d.trampoline = (uint8_t*)VirtualAlloc(nullptr, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!d.trampoline) return false;

    memcpy(d.origBytes,  target, total);
    memcpy(d.trampoline, target, total);
    // JMP rel32 back to (target + total)
    d.trampoline[total] = 0xE9;
    int32_t relBack = (int32_t)((uintptr_t)target + total
                              - ((uintptr_t)d.trampoline + total + 5));
    memcpy(d.trampoline + total + 1, &relBack, 4);

    DWORD oldProt;
    if (!VirtualProtect(target, total, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    uint8_t patch[16];
    memset(patch, 0x90, total);   // NOP-pad anything beyond our 5-byte JMP
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

// ----- D3D9 vtable indices -----
constexpr int VTBL_RESET    = 16;
constexpr int VTBL_ENDSCENE = 42;

using PFN_EndScene = HRESULT (WINAPI*)(IDirect3DDevice9*);
using PFN_Reset    = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static Detour       d_endScene{}, d_reset{};
static PFN_EndScene origEndScene = nullptr;
static PFN_Reset    origReset    = nullptr;
static void**       g_deviceVtbl = nullptr;

// ---- SetCursorPos / ClipCursor / SetCursor hooks -----------------------
// While the menu is open we need to:
//   1) Stop the game from recentering the cursor (SetCursorPos)
//   2) Stop the game from clipping the cursor to the viewport (ClipCursor)
//   3) Stop the game from *showing* a cursor (SetCursor -> NULL)
// Combined with ShowCursor(FALSE), this makes the OS cursor completely
// invisible so only the ImGui software cursor is drawn.
using PFN_SetCursorPos = BOOL (WINAPI*)(int, int);
using PFN_ClipCursor   = BOOL (WINAPI*)(const RECT*);
using PFN_SetCursor    = HCURSOR (WINAPI*)(HCURSOR);
static Detour          d_setCursorPos{}, d_clipCursor{}, d_setCursor{};
static PFN_SetCursorPos origSetCursorPos = nullptr;
static PFN_ClipCursor   origClipCursor   = nullptr;
static PFN_SetCursor    origSetCursor    = nullptr;

// Diagnostic: count how often each cursor hook fires.
static volatile LONG g_scpCalls = 0, g_ccCalls = 0, g_scCalls = 0;

static BOOL WINAPI HookedSetCursorPos(int x, int y) {
    InterlockedIncrement(&g_scpCalls);
    if (g_cfg.showMenu) return TRUE;
    return origSetCursorPos(x, y);
}

static BOOL WINAPI HookedClipCursor(const RECT* r) {
    InterlockedIncrement(&g_ccCalls);
    if (g_cfg.showMenu) return TRUE;  // let the cursor roam freely
    return origClipCursor(r);
}

static HCURSOR WINAPI HookedSetCursor(HCURSOR h) {
    InterlockedIncrement(&g_scCalls);
    // While the menu is open, force the OS cursor to NULL (invisible).
    if (g_cfg.showMenu) return origSetCursor(nullptr);
    return origSetCursor(h);
}

// ----- ImGui state -----
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
    // We feed input ourselves each frame; don't let the Win32 backend
    // try to read messages via its own WndProc hook or timer.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // MouseDrawCursor is toggled per-frame based on menu state
    // (see HookedEndScene), so we don't leave an ImGui cursor visible
    // when the menu is closed.
    io.MouseDrawCursor = false;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(gameHwnd);
    ImGui_ImplDX9_Init(dev);

    HLOG("  ImGui initialized on hwnd=%p", gameHwnd);
    imguiReady = true;
}

// Map a Win32 VK to an ImGuiKey for the keys we care about in widgets.
static ImGuiKey VkToImGuiKey(int vk) {
    switch (vk) {
        case VK_TAB:     return ImGuiKey_Tab;
        case VK_LEFT:    return ImGuiKey_LeftArrow;
        case VK_RIGHT:   return ImGuiKey_RightArrow;
        case VK_UP:      return ImGuiKey_UpArrow;
        case VK_DOWN:    return ImGuiKey_DownArrow;
        case VK_PRIOR:   return ImGuiKey_PageUp;
        case VK_NEXT:    return ImGuiKey_PageDown;
        case VK_HOME:    return ImGuiKey_Home;
        case VK_END:     return ImGuiKey_End;
        case VK_INSERT:  return ImGuiKey_Insert;
        case VK_DELETE:  return ImGuiKey_Delete;
        case VK_BACK:    return ImGuiKey_Backspace;
        case VK_RETURN:  return ImGuiKey_Enter;
        case VK_ESCAPE:  return ImGuiKey_Escape;
        case VK_SPACE:   return ImGuiKey_Space;
        case VK_LSHIFT:  return ImGuiKey_LeftShift;
        case VK_RSHIFT:  return ImGuiKey_RightShift;
        case VK_LCONTROL:return ImGuiKey_LeftCtrl;
        case VK_RCONTROL:return ImGuiKey_RightCtrl;
        case VK_LMENU:   return ImGuiKey_LeftAlt;
        case VK_RMENU:   return ImGuiKey_RightAlt;
        case VK_OEM_COMMA:  return ImGuiKey_Comma;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_MINUS:  return ImGuiKey_Minus;
        case VK_OEM_PLUS:   return ImGuiKey_Equal;
        case VK_OEM_1:   return ImGuiKey_Semicolon;
        case VK_OEM_2:   return ImGuiKey_Slash;
        case VK_OEM_3:   return ImGuiKey_GraveAccent;
        case VK_OEM_4:   return ImGuiKey_LeftBracket;
        case VK_OEM_5:   return ImGuiKey_Backslash;
        case VK_OEM_6:   return ImGuiKey_RightBracket;
        case VK_OEM_7:   return ImGuiKey_Apostrophe;
        default: break;
    }
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    return ImGuiKey_None;
}

// Poll cursor + keyboard state and push into ImGui IO. This avoids every
// problem a message-pump hook has: child windows, SetCapture, threads,
// focus races, timing of WM_MOUSEMOVE, etc.
static void FeedImGuiInput() {
    ImGuiIO& io = ImGui::GetIO();

    // --- Mouse position in client space of the game window ---
    POINT cp;
    if (GetCursorPos(&cp) && gameHwnd) {
        ScreenToClient(gameHwnd, &cp);
        io.AddMousePosEvent((float)cp.x, (float)cp.y);
    }

    // --- Mouse buttons (edge-detected so we only call Add* on change) ---
    static bool prevBtn[5] = { false, false, false, false, false };
    int vks[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 5; ++i) {
        bool down = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
        if (down != prevBtn[i]) {
            io.AddMouseButtonEvent(i, down);
            prevBtn[i] = down;
        }
    }

    // --- Full-range keyboard poll with edge detection ---
    // Build the kbdState table OURSELVES from GetAsyncKeyState rather than
    // trusting GetKeyboardState(). GetKeyboardState returns the input-queue
    // snapshot of the calling thread, and our render thread may not process
    // keyboard messages at all - Shift/Ctrl/CapsLock would always read 0.
    // GetAsyncKeyState is global and always reflects the real hardware.
    BYTE kbdState[256] = {};
    HKL hkl = GetKeyboardLayout(0);
    for (int v = 1; v < 256; ++v) {
        if ((GetAsyncKeyState(v) & 0x8000) != 0) kbdState[v] = 0x80;
    }

    // CapsLock/NumLock/ScrollLock toggle state:
    // GetKeyState is thread-local and returns 0 for our render thread.
    // GetAsyncKeyState only tells us if the key is physically down, not
    // the toggle state. So we track toggles ourselves by detecting the
    // down-edge of each lock key and flipping a sticky bit.
    static bool capsToggle = (GetKeyState(VK_CAPITAL) & 1) != 0;
    static bool numToggle  = (GetKeyState(VK_NUMLOCK) & 1) != 0;
    static bool scrollToggle = (GetKeyState(VK_SCROLL) & 1) != 0;
    static bool prevCapsDown = false, prevNumDown = false, prevScrollDown = false;
    bool capsDown   = (kbdState[VK_CAPITAL] & 0x80) != 0;
    bool numDown    = (kbdState[VK_NUMLOCK] & 0x80) != 0;
    bool scrollDown = (kbdState[VK_SCROLL]  & 0x80) != 0;
    if (capsDown   && !prevCapsDown)   capsToggle   = !capsToggle;
    if (numDown    && !prevNumDown)    numToggle    = !numToggle;
    if (scrollDown && !prevScrollDown) scrollToggle = !scrollToggle;
    prevCapsDown = capsDown;
    prevNumDown = numDown;
    prevScrollDown = scrollDown;
    if (capsToggle)   kbdState[VK_CAPITAL] |= 1;
    if (numToggle)    kbdState[VK_NUMLOCK] |= 1;
    if (scrollToggle) kbdState[VK_SCROLL]  |= 1;

    static bool prevKey[256] = {};

    for (int vk = 0x08; vk < 0xFF; ++vk) {
        // Skip mouse buttons (handled above)
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;

        bool down = (kbdState[vk] & 0x80) != 0;
        if (down == prevKey[vk]) continue;
        prevKey[vk] = down;

        // Key-down / key-up event for ImGui's own key tracking
        ImGuiKey ik = VkToImGuiKey(vk);
        if (ik != ImGuiKey_None) io.AddKeyEvent(ik, down);

        // Modifier flags (mirror state into ImGui's mod keys)
        if (vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT)
            io.AddKeyEvent(ImGuiMod_Shift,  (kbdState[VK_SHIFT]   & 0x80) != 0);
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
            io.AddKeyEvent(ImGuiMod_Ctrl,   (kbdState[VK_CONTROL] & 0x80) != 0);
        if (vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU)
            io.AddKeyEvent(ImGuiMod_Alt,    (kbdState[VK_MENU]    & 0x80) != 0);

        // On key press, translate to a character via the current layout
        // and feed InputText widgets. Shift/Caps are handled by kbdState.
        if (down) {
            UINT scan = MapVirtualKeyExA(vk, MAPVK_VK_TO_VSC, hkl);
            wchar_t buf[8] = {};
            int n = ToUnicodeEx((UINT)vk, scan, kbdState, buf,
                                (int)(sizeof(buf)/sizeof(buf[0])), 0, hkl);
            if (n > 0) {
                for (int i = 0; i < n; ++i) {
                    if (buf[i] >= 0x20 || buf[i] == '\t')
                        io.AddInputCharacterUTF16((unsigned short)buf[i]);
                }
            }
            // If ToUnicodeEx returned < 0 it set dead-key state; flush it with
            // a dummy call so subsequent keys aren't corrupted.
            if (n < 0) {
                wchar_t junk[4];
                ToUnicodeEx(VK_SPACE,
                            MapVirtualKeyExA(VK_SPACE, MAPVK_VK_TO_VSC, hkl),
                            kbdState, junk, 4, 0, hkl);
            }
        }
    }
}

// ----- INSERT toggle (polled in EndScene) -----
static void HandleToggleKey() {
    static bool prevDown = false;
    bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (down && !prevDown) g_cfg.showMenu = !g_cfg.showMenu;
    prevDown = down;
}

// ----- Cursor visibility management --------------------------------------
// Roblox 2009 uses the G3D engine's Win32Window, which sets the cursor
// ONCE via the window class (SetClassLongA with GCL_HCURSOR). After that,
// Windows auto-paints the class cursor on every WM_SETCURSOR and the game
// never calls user32!SetCursor, so our SetCursor hook does not help on its
// own. We additionally clear the class cursor on both the render HWND and
// its top-level parent while the menu is open, restoring both on close.
static int     osCursorHideDelta  = 0;   // matched ShowCursor(FALSE) calls
static HWND    scrubbedTopHwnd    = nullptr;
static HCURSOR savedTopCursor     = nullptr;
static HWND    scrubbedChildHwnd  = nullptr;
static HCURSOR savedChildCursor   = nullptr;
static bool    loggedClassInfo    = false;

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

    // One-time diagnostic: log the window hierarchy and the class cursor
    // of each HWND we can reach, so we can see where Roblox set it.
    if (!loggedClassInfo && gameHwnd) {
        HWND top = GetTopLevelGameWindow();
        char cls[128] = {};
        GetClassNameA(gameHwnd, cls, sizeof(cls));
        HLOG("  gameHwnd=%p class=%s classCursor=%p",
             gameHwnd, cls, (void*)GetClassLongPtrA(gameHwnd, -12));
        if (top && top != gameHwnd) {
            GetClassNameA(top, cls, sizeof(cls));
            HLOG("  topHwnd=%p class=%s classCursor=%p",
                 top, cls, (void*)GetClassLongPtrA(top, -12));
        }
        loggedClassInfo = true;
    }

    if (menuOpen) {
        io.MouseDrawCursor = true;

        // Scrub the class cursor on BOTH the child (render) window and
        // the top-level window, since Roblox may have set it on either.
        HWND top = GetTopLevelGameWindow();
        if (!scrubbedChildHwnd && gameHwnd) {
            scrubbedChildHwnd = gameHwnd;
            savedChildCursor = (HCURSOR)SetClassLongPtrA(gameHwnd, -12, 0);
        }
        if (!scrubbedTopHwnd && top && top != gameHwnd) {
            scrubbedTopHwnd = top;
            savedTopCursor = (HCURSOR)SetClassLongPtrA(top, -12, 0);
        }

        // Force the cursor invisible right now, in case the game isn't
        // about to call SetCursor on its own.
        SetCursor(nullptr);

        // Drive the global ShowCursor counter negative.
        if (osCursorHideDelta == 0) {
            int c = ShowCursor(FALSE);
            osCursorHideDelta = 1;
            while (c >= 0) {
                c = ShowCursor(FALSE);
                osCursorHideDelta++;
            }
        }
    } else {
        io.MouseDrawCursor = false;

        // Restore class cursors on whichever HWNDs we scrubbed.
        if (scrubbedChildHwnd) {
            SetClassLongPtrA(scrubbedChildHwnd, -12, (LONG_PTR)savedChildCursor);
            scrubbedChildHwnd = nullptr;
            savedChildCursor = nullptr;
        }
        if (scrubbedTopHwnd) {
            SetClassLongPtrA(scrubbedTopHwnd, -12, (LONG_PTR)savedTopCursor);
            scrubbedTopHwnd = nullptr;
            savedTopCursor = nullptr;
        }

        while (osCursorHideDelta > 0) {
            ShowCursor(TRUE);
            osCursorHideDelta--;
        }
    }
}

static HRESULT WINAPI HookedEndScene(IDirect3DDevice9* dev) {
    InitImGuiOnce(dev);

    // Drain executor on the game's render thread
    executor::Tick();

    // Get backbuffer dims
    int sw = 0, sh = 0;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC desc; bb->GetDesc(&desc);
        sw = (int)desc.Width; sh = (int)desc.Height;
        bb->Release();
    }

    HandleToggleKey();
    UpdateOsCursorVisibility(g_cfg.showMenu);

    // Periodically log hook-fire counts so we can see whether SetCursor
    // is actually being called per-frame. Once per ~5 seconds.
    static DWORD lastLog = 0;
    DWORD now = GetTickCount();
    if (now - lastLog > 5000) {
        lastLog = now;
        HLOG("  hook fires: SetCursorPos=%ld ClipCursor=%ld SetCursor=%ld",
             g_scpCalls, g_ccCalls, g_scCalls);
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Feed input BEFORE NewFrame() consumes the io state.
    if (g_cfg.showMenu) FeedImGuiInput();

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
    HRESULT hr = origReset(dev, pp);
    if (imguiReady && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// ----- Vtable discovery via dummy device -----
static void** GetD3D9DeviceVTable() {
    HLOG("  Direct3DCreate9...");
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { HLOG("  Direct3DCreate9 returned null"); return nullptr; }
    HLOG("  d3d=%p", d3d);

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) hwnd = GetDesktopWindow();
    HLOG("  hwnd=%p", hwnd);

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow    = hwnd;

    IDirect3DDevice9* dev = nullptr;
    HLOG("  CreateDevice (HAL)...");
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                   D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                   &pp, &dev);
    HLOG("  CreateDevice HAL: hr=0x%08X dev=%p", hr, dev);
    if (FAILED(hr) || !dev) {
        HLOG("  CreateDevice (NULLREF fallback)...");
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        HLOG("  CreateDevice NULLREF: hr=0x%08X dev=%p", hr, dev);
    }
    if (FAILED(hr) || !dev) { d3d->Release(); return nullptr; }

        void** vtbl = *reinterpret_cast<void***>(dev);
    HLOG("  vtbl=%p  EndScene=%p  Reset=%p",
         vtbl, vtbl[VTBL_ENDSCENE], vtbl[VTBL_RESET]);

    // Dump the first 16 bytes of EndScene so we can see its prologue
    uint8_t* es = (uint8_t*)vtbl[VTBL_ENDSCENE];
    HLOG("  EndScene prologue: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
         es[0], es[1], es[2], es[3], es[4], es[5], es[6], es[7], es[8], es[9]);

    // Intentionally do NOT release the dummy device / d3d object.
    // Releasing a HAL device created on a borrowed HWND (often the console)
    // with DISABLE_DRIVER_MANAGEMENT can crash the graphics driver on teardown.
    // The function pointers we grabbed are inside d3d9.dll and remain valid
    // regardless. The one-time leak is trivial.
    HLOG("  (leaking dummy device; vtable pointers are in d3d9.dll and stable)");
    return vtbl;
}

bool Install() {
    HLOG("hooks::Install entry");
    g_deviceVtbl = GetD3D9DeviceVTable();
    if (!g_deviceVtbl) { HLOG("  no vtable"); return false; }

    origEndScene = (PFN_EndScene)g_deviceVtbl[VTBL_ENDSCENE];
    origReset    = (PFN_Reset)   g_deviceVtbl[VTBL_RESET];
    HLOG("  origEndScene=%p origReset=%p", origEndScene, origReset);

    HLOG("  inline hooking EndScene at %p...", origEndScene);
    if (!InstallInlineHook(d_endScene, (void*)origEndScene, (void*)&HookedEndScene)) {
        HLOG("  EndScene hook failed");
        return false;
    }
    origEndScene = (PFN_EndScene)d_endScene.trampoline;

    HLOG("  inline hooking Reset at %p...", origReset);
    if (!InstallInlineHook(d_reset, (void*)origReset, (void*)&HookedReset)) {
        HLOG("  Reset hook failed");
        return false;
    }
    origReset = (PFN_Reset)d_reset.trampoline;

    // Hook SetCursorPos / ClipCursor / SetCursor so the game can't fight
    // our ImGui cursor or force an OS cursor back on.
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
            HLOG("  user32!%s = %p", s.name, p);
            if (!p) continue;
            uint8_t* b = (uint8_t*)p;
            HLOG("    bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                 b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
            void* real = p;
            for (int i = 0; i < 4 && real; ++i) {
                void* next = FollowJmpStub(real);
                if (next == real) break;
                HLOG("    forwarder %p -> %p", real, next);
                real = next;
            }
            if (real && InstallInlineHook(*s.det, real, s.detour)) {
                *s.pOrig = s.det->trampoline;
                HLOG("    %s hooked @%p trampoline=%p", s.name, real, s.det->trampoline);
            } else {
                HLOG("    %s hook FAILED", s.name);
            }
        }
    }

    HLOG("  hooks installed; trampolines at %p, %p",
         d_endScene.trampoline, d_reset.trampoline);
    return true;
}

void Uninstall() {
    UninstallInlineHook(d_endScene);
    UninstallInlineHook(d_reset);
    UninstallInlineHook(d_setCursorPos);
    UninstallInlineHook(d_clipCursor);
    UninstallInlineHook(d_setCursor);
    if (imguiReady) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady = false;
    }
}

} // namespace hooks
