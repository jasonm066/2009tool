// ============================================================
//  Lua 5.1 API + Roblox internals — RobloxApp_client.exe (2009E)
//  MD5: 8c5dae1726dab66f4cf212ac36fafed2  imagebase: 0x00400000
// ============================================================
#pragma once
#include <cstdint>
#include <Windows.h>

namespace rva {
    constexpr uintptr_t lua_gettop      = 0x297B70;
    constexpr uintptr_t lua_settop      = 0x297B80;
    constexpr uintptr_t lua_pushvalue   = 0x297D30;
    constexpr uintptr_t lua_type        = 0x297D60;
    constexpr uintptr_t lua_tolstring   = 0x29C1C0;
    constexpr uintptr_t lua_pushlstring = 0x298170;
    constexpr uintptr_t lua_pushboolean = 0x298320;
    constexpr uintptr_t lua_pcall       = 0x2988F0;
    constexpr uintptr_t lua_load        = 0x298960;
    constexpr uintptr_t lua_newthread   = 0x298F80;
    constexpr uintptr_t luaL_loadbuffer = 0x299860;
}

constexpr int LUA_GLOBALSINDEX = -10002;

using lua_State = void;
using pfn_lua_gettop      = int        (__cdecl*)(lua_State*);
using pfn_lua_settop      = void       (__cdecl*)(lua_State*, int);
using pfn_lua_tolstring   = const char*(__cdecl*)(lua_State*, int, size_t*);
using pfn_lua_pushlstring = void       (__cdecl*)(lua_State*, const char*, size_t);
using pfn_lua_pushboolean = void       (__cdecl*)(lua_State*, int);
using pfn_lua_pcall       = int        (__cdecl*)(lua_State*, int, int, int);
using pfn_luaL_loadbuffer = int        (__cdecl*)(lua_State*, const char*, size_t, const char*);

struct LuaAPI {
    HMODULE   base     = nullptr;
    uintptr_t baseAddr = 0;

    pfn_lua_gettop      gettop      = nullptr;
    pfn_lua_settop      settop      = nullptr;
    pfn_lua_tolstring   tolstring   = nullptr;
    pfn_lua_pushlstring pushlstring = nullptr;
    pfn_lua_pushboolean pushboolean = nullptr;
    pfn_lua_pcall       pcall       = nullptr;
    pfn_luaL_loadbuffer loadbuffer  = nullptr;

    bool Init() {
        base = GetModuleHandleA("RobloxApp_client.exe");
        if (!base) return false;
        baseAddr = reinterpret_cast<uintptr_t>(base);
        auto R = [&](uintptr_t r) { return baseAddr + r; };
        gettop      = (pfn_lua_gettop)      R(rva::lua_gettop);
        settop      = (pfn_lua_settop)      R(rva::lua_settop);
        tolstring   = (pfn_lua_tolstring)   R(rva::lua_tolstring);
        pushlstring = (pfn_lua_pushlstring) R(rva::lua_pushlstring);
        pushboolean = (pfn_lua_pushboolean) R(rva::lua_pushboolean);
        pcall       = (pfn_lua_pcall)       R(rva::lua_pcall);
        loadbuffer  = (pfn_luaL_loadbuffer) R(rva::luaL_loadbuffer);
        return true;
    }
};
