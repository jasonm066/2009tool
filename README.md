# 2009 Roblox - Internal All-in-One

Single injected DLL combining the ESP overlay and the Lua script executor.
No external overlay window, no focus juggling - ImGui draws directly into
the game's D3D9 backbuffer via an EndScene hook.

## Build

Open **x86 Native Tools Command Prompt for VS**, cd here, run:
```
build.bat
```
Outputs `internal.dll` and `injector.exe`.

## Use

1. Launch the 2009 Roblox client and **join a place**.
2. Run `injector.exe`.
3. Press **INSERT** in the game window to toggle the menu.
4. **ESP** tab - same toggles as the external version.
5. **Executor** tab - paste/edit Lua, click *Execute*. Output (errors, logs) shows below.

## How it works

- `hooks.cpp` creates a throwaway D3D9 device to read its vtable, then patches
  5-byte JMPs over `EndScene` (slot 42) and `Reset` (slot 16). Trampolines hold
  the original 5 bytes plus a JMP back, so `origEndScene(dev)` works as expected.
- `EndScene` hook runs every frame on the render thread - it draws ImGui,
  the ESP, and drains queued Lua scripts (`executor::Tick`).
- The window's WndProc is subclassed so input goes to ImGui when the menu is
  open, with mouse/keyboard events swallowed before reaching the game.
- Lua scripts run on the same thread as Roblox itself uses - no thread safety
  issues, unlike the standalone executor.

## Files

```
internal/
  dllmain.cpp     entry point + init thread
  hooks.cpp/h     D3D9 vtable hooks + WndProc subclass
  esp.cpp/h       in-process ESP draw (port of src/main.cpp's DrawESP)
  menu.cpp/h      ImGui tabs (ESP + Executor)
  executor.cpp/h  Lua loadbuffer + pcall queue + log buffer
  config.h        feature toggles + save/load
  draw.h          ImGui drawing primitives
  roblox.h        in-process pointer chains + struct readers
  lua_api.h       hard-coded RVAs for Lua functions in the client
  injector.cpp    CreateRemoteThread + LoadLibraryA loader
  build.bat       single-script build
```
