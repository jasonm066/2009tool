#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include "types.h"

// ============================================================
//  In-process Roblox 2009 memory access.
//  All pointers dereferenced directly (we live in the game).
// ============================================================

namespace offsets {
    // Instance
    constexpr uint32_t Primitive = 0x24;
    constexpr uint32_t Children  = 0x50;
    constexpr uint32_t Name      = 0x58;
    constexpr uint32_t Character = 0x8C;
    // Primitive
    constexpr uint32_t PosX      = 0x164;
    constexpr uint32_t PosY      = 0x168;
    constexpr uint32_t PosZ      = 0x16C;
    // Humanoid
    constexpr uint32_t Health    = 0x19C;
    constexpr uint32_t MaxHealth = 0x1A0;
    // Camera
    constexpr uint32_t CamFOV       = 0x90;
    constexpr uint32_t CamRotation  = 0xA0;
    constexpr uint32_t CamPosX      = 0xC4;
    // ScriptContext
    constexpr uint32_t SC_LuaState  = 0x8C;
}

// ---- Pointer chain bases (from RobloxApp_client.exe imagebase) ----
namespace chains {
    constexpr uint32_t DM_BASE      = 0x603730;   // shared by Players + ScriptContext
    constexpr uint32_t CAMERA_BASE  = 0x619574;
}

// ---- Safe dereference utilities ----
template <typename T>
inline T SafeDeref(uintptr_t addr, T fallback = T{}) {
    __try { return *reinterpret_cast<T*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}

inline uintptr_t WalkChain(uintptr_t base, const uint32_t* offs, size_t n) {
    uintptr_t addr = base;
    __try {
        for (size_t i = 0; i < n; i++) {
            uintptr_t val = *reinterpret_cast<uintptr_t*>(addr);
            addr = val + offs[i];
        }
        return addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- Specific resolvers ----
inline uintptr_t GetPlayers(uintptr_t modBase) {
    static const uint32_t CHAIN[] = { 0x4C, 0xC, 0x40, 0x0 };
    return WalkChain(modBase + chains::DM_BASE, CHAIN, 4);
}
inline uintptr_t GetScriptContext(uintptr_t modBase) {
    static const uint32_t CHAIN[] = { 0x4C, 0xC, 0x8, 0x0 };
    return WalkChain(modBase + chains::DM_BASE, CHAIN, 4);
}
inline uintptr_t GetCameraInst(uintptr_t modBase) {
    static const uint32_t CHAIN[] = { 0x148, 0x10C, 0x4C, 0xC, 0x0, 0x0 };
    return WalkChain(modBase + chains::CAMERA_BASE, CHAIN, 6);
}

// ---- String reading (MSVC SSO) ----
inline bool ReadString(uintptr_t strAddr, char* buf, size_t sz) {
    memset(buf, 0, sz);
    __try {
        uint32_t cap = *reinterpret_cast<uint32_t*>(strAddr + 0x14);
        if (cap < 16) {
            memcpy(buf, reinterpret_cast<void*>(strAddr), 15);
        } else {
            uint32_t ptr = *reinterpret_cast<uint32_t*>(strAddr);
            if (!ptr) return false;
            strncpy(buf, reinterpret_cast<const char*>(ptr), sz - 1);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Children list ----
inline uint32_t GetChildren(uint32_t inst, uint32_t* out, uint32_t maxN) {
    __try {
        uint32_t A = *reinterpret_cast<uint32_t*>(inst + offsets::Children);
        if (!A) return 0;
        uint32_t B = *reinterpret_cast<uint32_t*>(A + 0xC);
        if (!B) return 0;
        uint32_t s = *reinterpret_cast<uint32_t*>(B + 0xC);
        uint32_t e = *reinterpret_cast<uint32_t*>(B + 0x10);
        if (e < s) return 0;
        uint32_t count = (e - s) / 8;
        if (count > maxN) count = maxN;
        for (uint32_t i = 0; i < count; i++)
            out[i] = *reinterpret_cast<uint32_t*>(s + i * 8);
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline uint32_t FindChild(uint32_t inst, const char* target) {
    uint32_t children[64];
    uint32_t n = GetChildren(inst, children, 64);
    for (uint32_t i = 0; i < n; i++) {
        if (!children[i]) continue;
        char nm[64];
        if (!ReadString(children[i] + offsets::Name, nm, sizeof(nm))) continue;
        if (strcmp(nm, target) == 0) return children[i];
    }
    return 0;
}

// ---- Position / Health / Camera ----
inline bool GetPartPosition(uint32_t part, Vec3& out) {
    __try {
        uint32_t prim = *reinterpret_cast<uint32_t*>(part + offsets::Primitive);
        if (!prim) return false;
        out = *reinterpret_cast<Vec3*>(prim + offsets::PosX);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool ReadHealth(uint32_t humanoid, float& hp, float& maxHP) {
    __try {
        float* p = reinterpret_cast<float*>(humanoid + offsets::Health);
        hp    = p[0];
        maxHP = p[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool ReadCamera(uintptr_t cam, Camera& out) {
    if (!cam) return false;
    __try {
        memcpy(out.rot,  reinterpret_cast<void*>(cam + offsets::CamRotation), 36);
        memcpy(&out.pos, reinterpret_cast<void*>(cam + offsets::CamPosX),     12);
        out.fov = *reinterpret_cast<float*>(cam + offsets::CamFOV);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
