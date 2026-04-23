#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include "types.h"

namespace offsets {
    // Instance
    constexpr uint32_t Primitive     = 0x24;
    constexpr uint32_t Parent        = 0x48;
    constexpr uint32_t Children      = 0x50;
    constexpr uint32_t Name          = 0x58;
    constexpr uint32_t Character     = 0x8C;
    // PartInstance
    constexpr uint32_t BrickColor    = 0x180;
    constexpr uint32_t Reflectance   = 0x184;
    constexpr uint32_t Transparency  = 0x188;
    constexpr uint32_t SizeX         = 0x1D4;
    constexpr uint32_t SizeY         = 0x1D8;
    constexpr uint32_t SizeZ         = 0x1DC;
    // Primitive (relative to Primitive ptr)
    constexpr uint32_t PosX      = 0x164;
    constexpr uint32_t PosY      = 0x168;
    constexpr uint32_t PosZ      = 0x16C;
    // Humanoid
    constexpr uint32_t Health    = 0x19C;
    constexpr uint32_t MaxHealth = 0x1A0;
    // Camera
    constexpr uint32_t CamFOV      = 0x90;
    constexpr uint32_t CamRotation = 0xA0;
    constexpr uint32_t CamPosX     = 0xC4;
    // ScriptContext
    constexpr uint32_t SC_LuaState = 0x8C;
}

namespace chains {
    constexpr uint32_t DM_BASE     = 0x603730; // shared by Players + ScriptContext
    constexpr uint32_t CAMERA_BASE = 0x619574;
}

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

// *(*(modBase + 0x603730) + 0x24)  — from Cheat Engine table
inline uintptr_t GetDataModel(uintptr_t modBase) {
    static const uint32_t CHAIN[] = { 0x24, 0x0 };
    return WalkChain(modBase + chains::DM_BASE, CHAIN, 2);
}
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

// MSVC SSO: cap < 16 means string data is stored inline at strAddr
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

// 32-byte string struct used by Roblox's reflection tables.
// Layout: +0=heap ptr (when cap>=16), +4=inline buffer (15+null), +0x14=length, +0x18=capacity
inline bool ReadStdString(uintptr_t structAddr, char* buf, size_t sz) {
    if (!buf || sz == 0) return false;
    buf[0] = '\0';
    __try {
        uint32_t len = *reinterpret_cast<uint32_t*>(structAddr + 0x14);
        uint32_t cap = *reinterpret_cast<uint32_t*>(structAddr + 0x18);
        if (len > 4096 || cap > 0x10000) return false;
        const char* src;
        if (cap < 16) {
            src = reinterpret_cast<const char*>(structAddr + 4);
        } else {
            uint32_t ptr = *reinterpret_cast<uint32_t*>(structAddr);
            if (!ptr) return false;
            src = reinterpret_cast<const char*>(ptr);
        }
        size_t n = (len < sz - 1) ? len : sz - 1;
        for (size_t i = 0; i < n; i++) {
            char c = src[i];
            buf[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        buf[n] = '\0';
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

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

inline int GetChildCount(uint32_t inst) {
    __try {
        uint32_t A = *reinterpret_cast<uint32_t*>(inst + offsets::Children);
        if (!A) return 0;
        uint32_t B = *reinterpret_cast<uint32_t*>(A + 0xC);
        if (!B) return 0;
        uint32_t s = *reinterpret_cast<uint32_t*>(B + 0xC);
        uint32_t e = *reinterpret_cast<uint32_t*>(B + 0x10);
        return (e >= s) ? (int)((e - s) / 8) : 0;
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

// Reads Roblox engine class name via reflection descriptor at inst+0x1C
// e.g. "Part", "Model", "Humanoid", "Workspace", "DataModel"
inline bool ReadClassName(uintptr_t inst, char* buf, size_t sz) {
    buf[0] = '\0';
    __try {
        uintptr_t classDesc = *reinterpret_cast<uintptr_t*>(inst + 0x1C);
        if (!classDesc) return false;
        uintptr_t nameStruct = *reinterpret_cast<uintptr_t*>(classDesc + 0x04);
        if (!nameStruct) return false;
        const char* src = reinterpret_cast<const char*>(nameStruct + 0x04);
        // Validate: must start with uppercase ASCII letter
        if (src[0] < 'A' || src[0] > 'Z') return false;
        size_t i = 0;
        while (i < sz - 1 && src[i] >= 0x20 && src[i] < 0x7F) { buf[i] = src[i]; i++; }
        buf[i] = '\0';
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Property kinds we can decode from getter thunks
enum PropKind : uint8_t {
    PK_UNKNOWN = 0,
    PK_FLOAT,             // *(float*)(inst + offset)
    PK_INT32,             // *(int32*)(inst + offset)
    PK_BOOL,              // *(uint8*)(inst + offset)
    PK_VECTOR3,           // 3 floats at (inst + offset)
    PK_STRING,            // std::string struct at (inst + offset)
    PK_CONST_BOOL,        // constant value held in `offset` (0 or 1)
    PK_INDIRECT_FLOAT,    // *(float*)(*(uintptr*)(inst + offset) + offset2)
    PK_INDIRECT_INT32,    // *(int32*)(*(uintptr*)(inst + offset) + offset2)
    PK_INDIRECT_BOOL,     // *(uint8*)(*(uintptr*)(inst + offset) + offset2)
    PK_INDIRECT_VECTOR3,  // 3 floats at (*(uintptr*)(inst + offset) + offset2)
    PK_DEREF_VECTOR3,     // 3 floats at *(*(uintptr*)(*(uintptr*)(inst+offset)+offset2))
    PK_DEREF_VECTOR3_P4,  // like DEREF_VECTOR3 but reads from vecPtr+4 (Part Size getter)
    PK_POSITION,          // world position via prim chain: *(Vec3*)(*(inst+0x24)+0x164)
    PK_BITFIELD_BOOL,     // (*(uint8*)(inst+offset) & offset2) != 0  — single bit from packed byte
    PK_VTABLE_RELAY,      // vtable dispatch: offset=slot_byte; resolved to real kind at read-time
};

enum PropFlags : uint8_t {
    PF_NONE      = 0,
    PF_COLORNAME = 1 << 0, // int32 value renders via BrickColorName()
};

struct PropEntry {
    char     name[48];
    char     category[24];
    uint8_t  kind;
    uint8_t  flags;
    uint32_t offset;
    uint32_t offset2;
};

// Decode a getter thunk to determine the kind and field offset.
// Returns true if the pattern is recognized.
inline bool DecodePropGetter(uintptr_t getter, uint8_t& kind, uint32_t& offset, uint32_t& offset2) {
    if (!getter) return false;
    uint8_t b[28] = {};
    __try { memcpy(b, reinterpret_cast<void*>(getter), 28); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    // -------- Jmp-thunk resolution (incremental linker stubs / MSVC this-adjust thunks) --------
    // E9 [rel32] — simple near-jmp forwarding stub
    if (b[0] == 0xE9) {
        int32_t rel = *reinterpret_cast<int32_t*>(b + 1);
        getter = getter + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        __try { memcpy(b, reinterpret_cast<void*>(getter), 28); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // B8 [imm32] E9 [rel32] — MSVC this-adjustment thunk: mov eax,imm; jmp rel
    else if (b[0] == 0xB8 && b[5] == 0xE9) {
        int32_t rel = *reinterpret_cast<int32_t*>(b + 6);
        getter = (getter + 5) + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        __try { memcpy(b, reinterpret_cast<void*>(getter), 28); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // -------- Position via prim chain (8B 89 [off1:32] E8 ?? ?? ?? ?? 83 C0 [add:8] C3) --------
    // mov ecx,[ecx+off]; call fn; add eax,add; ret — complex vtable route, use known prim formula
    if (b[0] == 0x8B && b[1] == 0x89 && b[6] == 0xE8 &&
        b[11] == 0x83 && b[12] == 0xC0 && b[14] == 0xC3) {
        kind = PK_POSITION; return true;
    }

    // -------- Color3 / Vector3 returned via hidden pointer + FPU store x3 --------
    // 8B 44 24 04 D9 81 [off:32] D9 18 D9 81 ...
    // mov eax,[esp+4]; fld [ecx+off]; fstp [eax]; fld [ecx+off+4]; fstp [eax+4]; ...
    if (b[0] == 0x8B && b[1] == 0x44 && b[2] == 0x24 && b[3] == 0x04 &&
        b[4] == 0xD9 && b[5] == 0x81 && b[10] == 0xD9 && b[11] == 0x18) {
        offset = *reinterpret_cast<uint32_t*>(b + 6);
        kind = PK_VECTOR3; return true;
    }

    // -------- Bitfield bool via shr 1 (8A 81 [off:32] D0 E8 24 01 C3) --------
    // mov al,[ecx+off]; shr al,1; and al,1; ret — bit 1 of packed byte (e.g. Jump)
    if (b[0] == 0x8A && b[1] == 0x81 &&
        b[6] == 0xD0 && b[7] == 0xE8 && b[8] == 0x24 && b[9] == 0x01 && b[10] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = 1u << 1;
        kind = PK_BITFIELD_BOOL; return true;
    }
    // -------- Bitfield bool via shr N (8A 81 [off:32] C0 E8 [n:8] 24 01 C3) --------
    // mov al,[ecx+off]; shr al,n; and al,1; ret — bit N of packed byte (e.g. Sit)
    if (b[0] == 0x8A && b[1] == 0x81 &&
        b[6] == 0xC0 && b[7] == 0xE8 && b[9] == 0x24 && b[10] == 0x01 && b[11] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = 1u << b[8];
        kind = PK_BITFIELD_BOOL; return true;
    }

    // -------- String returned via copy (51 8B 91 [off:32] ...) --------
    // push ecx; mov edx,[ecx+off]; ... string copy via hidden ptr
    if (b[0] == 0x51 && b[1] == 0x8B && b[2] == 0x91) {
        offset = *reinterpret_cast<uint32_t*>(b + 3);
        kind = PK_STRING; return true;
    }
    // -------- String via hidden-ptr + lea esi (51 56 57 8B 7C 24 10 8D B1 [off:32]) --------
    // push ecx; push esi; push edi; mov edi,[esp+10]; lea esi,[ecx+off]; ...
    // Used by SoundId, MeshId, TextureId, Texture, LinkedSource
    if (b[0] == 0x51 && b[1] == 0x56 && b[2] == 0x57 &&
        b[3] == 0x8B && b[4] == 0x7C && b[5] == 0x24 && b[6] == 0x10 &&
        b[7] == 0x8D && b[8] == 0xB1) {
        offset = *reinterpret_cast<uint32_t*>(b + 9);
        kind = PK_STRING; return true;
    }

    // -------- Direct field accessors --------
    // D9 81 [off32] C3       fld dword ptr [ecx+off]; ret
    if (b[0] == 0xD9 && b[1] == 0x81 && b[6] == 0xC3) {
        offset = *reinterpret_cast<uint32_t*>(b + 2);
        kind = PK_FLOAT; return true;
    }
    // 8B 81 [off32] C3       mov eax, [ecx+off]; ret
    if (b[0] == 0x8B && b[1] == 0x81 && b[6] == 0xC3) {
        offset = *reinterpret_cast<uint32_t*>(b + 2);
        kind = PK_INT32; return true;
    }
    // 8A 81 [off32] C3       mov al, [ecx+off]; ret   (packed bool)
    if (b[0] == 0x8A && b[1] == 0x81 && b[6] == 0xC3) {
        offset = *reinterpret_cast<uint32_t*>(b + 2);
        kind = PK_BOOL; return true;
    }
    // 8D 81 [off32] C3       lea eax, [ecx+off]; ret
    if (b[0] == 0x8D && b[1] == 0x81 && b[6] == 0xC3) {
        offset = *reinterpret_cast<uint32_t*>(b + 2);
        kind = PK_VECTOR3; return true;
    }
    // 8D 41 [off8] C3        lea eax, [ecx+disp8]; ret  (small string offset)
    if (b[0] == 0x8D && b[1] == 0x41 && b[3] == 0xC3) {
        offset = b[2];
        kind = PK_STRING; return true;
    }

    // -------- Constant returns --------
    // B0 [val:8] C3          mov al, val; ret   (constant bool)
    if (b[0] == 0xB0 && b[2] == 0xC3) {
        offset = b[1] ? 1 : 0;
        kind = PK_CONST_BOOL; return true;
    }

    // -------- thiscall return-via-pointer (eax = [esp+4]) for value types --------
    // 8B 89 [off32] 8B 44 24 04 89 08 C2 04 00     mov ecx,[ecx+off]; mov eax,[esp+4]; mov [eax],ecx; ret 4
    if (b[0] == 0x8B && b[1] == 0x89 &&
        b[6] == 0x8B && b[7] == 0x44 && b[8] == 0x24 && b[9] == 0x04 &&
        b[10] == 0x89 && b[11] == 0x08 && b[12] == 0xC2 && b[13] == 0x04 && b[14] == 0x00) {
        offset = *reinterpret_cast<uint32_t*>(b + 2);
        kind = PK_INT32; return true;
    }

    // -------- Part Size: hidden-ptr + FPU copy of vec at vecPtr+4 --------
    // 8B 81 [off1:32] 8B 88 [off2:32] 8B 44 24 04 D9 41 04 83 C1 04 D9 18
    // mov eax,[ecx+off1]; mov ecx,[eax+off2]; mov eax,[esp+4]; fld [ecx+4]; add ecx,4; fstp [eax]; ...
    if (b[0]==0x8B && b[1]==0x81 &&
        b[6]==0x8B && b[7]==0x88 &&
        b[12]==0x8B && b[13]==0x44 && b[14]==0x24 && b[15]==0x04 &&
        b[16]==0xD9 && b[17]==0x41 && b[18]==0x04) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = *reinterpret_cast<uint32_t*>(b + 8);
        kind = PK_DEREF_VECTOR3_P4; return true;
    }

    // -------- Hidden-ptr return of deref'd vector pointer (e.g. SpawnLocation Size) --------
    // 8B 81 [off1:32] 8B 88 [off2:32] 8B 44 24 04 89 08 C2 04 00
    // mov eax,[ecx+off1]; mov ecx,[eax+off2]; *ret_ptr=ecx; ret 4
    // → body=*(inst+off1), vecPtr=*(body+off2), Vec3 at vecPtr+4
    if (b[0]==0x8B && b[1]==0x81 &&
        b[6]==0x8B && b[7]==0x88 &&
        b[12]==0x8B && b[13]==0x44 && b[14]==0x24 && b[15]==0x04 &&
        b[16]==0x89 && b[17]==0x08 && b[18]==0xC2 && b[19]==0x04 && b[20]==0x00) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = *reinterpret_cast<uint32_t*>(b + 8);
        kind = PK_DEREF_VECTOR3; return true;
    }

    // -------- Vtable dispatch (8B 01 FF 60 [slot8]) — resolve real getter at read-time --------
    // mov eax,[ecx]; jmp [eax+slot8] — stores slot byte in `offset`; ReadValue follows it live
    if (b[0] == 0x8B && b[1] == 0x01 && b[2] == 0xFF && b[3] == 0x60) {
        offset = b[4];
        kind = PK_VTABLE_RELAY; return true;
    }
    // -------- Vtable dispatch (8B 01 FF A0 [slot32]) — jmp [eax+imm32] variant --------
    if (b[0] == 0x8B && b[1] == 0x01 && b[2] == 0xFF && b[3] == 0xA0) {
        offset = *reinterpret_cast<uint32_t*>(b + 4);
        kind = PK_VTABLE_RELAY; return true;
    }

    // -------- Indirect via secondary pointer — disp8 variants --------
    // 8B 81 [off1:32] D9 40 [off2:8] C3   load ptr, fld float at disp8
    if (b[0] == 0x8B && b[1] == 0x81 &&
        b[6] == 0xD9 && b[7] == 0x40 && b[9] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = b[8];
        kind = PK_INDIRECT_FLOAT; return true;
    }
    // 8B 81 [off1:32] 8B 40 [off2:8] C3   load ptr, mov eax at disp8
    if (b[0] == 0x8B && b[1] == 0x81 &&
        b[6] == 0x8B && b[7] == 0x40 && b[9] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = b[8];
        kind = PK_INDIRECT_INT32; return true;
    }
    // 8B 81 [off1:32] 8A 40 [off2:8] C3   load ptr, mov al at disp8
    if (b[0] == 0x8B && b[1] == 0x81 &&
        b[6] == 0x8A && b[7] == 0x40 && b[9] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = b[8];
        kind = PK_INDIRECT_BOOL; return true;
    }

    // -------- Indirect via secondary pointer (e.g. Anchored via Primitive) --------
    // 8B 81 [off1:32] D9 80 [off2:32] C3   load primitive ptr, fld at +off2
    if (b[0] == 0x8B && b[1] == 0x81 &&
        b[6] == 0xD9 && b[7] == 0x80 && b[12] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = *reinterpret_cast<uint32_t*>(b + 8);
        kind = PK_INDIRECT_FLOAT; return true;
    }
    // 8B 81 [off1:32] 8B 80 [off2:32] C3   load primitive ptr, mov eax,[+off2]
    if (b[0] == 0x8B && b[1] == 0x81 &&
        b[6] == 0x8B && b[7] == 0x80 && b[12] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = *reinterpret_cast<uint32_t*>(b + 8);
        kind = PK_INDIRECT_INT32; return true;
    }
    // 8B 81 [off1:32] 8A 80 [off2:32] C3   load primitive ptr, mov al,[+off2]
    if (b[0] == 0x8B && b[1] == 0x81 &&
        b[6] == 0x8A && b[7] == 0x80 && b[12] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = *reinterpret_cast<uint32_t*>(b + 8);
        kind = PK_INDIRECT_BOOL; return true;
    }
    // 8B 81 [off1:32] 8D 80 [off2:32] C3   load primitive ptr, lea eax,[+off2]  (Vector3)
    if (b[0] == 0x8B && b[1] == 0x81 &&
        b[6] == 0x8D && b[7] == 0x80 && b[12] == 0xC3) {
        offset  = *reinterpret_cast<uint32_t*>(b + 2);
        offset2 = *reinterpret_cast<uint32_t*>(b + 8);
        kind = PK_INDIRECT_VECTOR3; return true;
    }

    return false;
}

// Walks classDesc's property descriptor vector and fills `out`.
// Returns the number of entries written.
inline int EnumerateProperties(uintptr_t inst, PropEntry* out, int maxN) {
    if (!inst || !out || maxN <= 0) return 0;
    int written = 0;
    __try {
        uintptr_t cd = *reinterpret_cast<uintptr_t*>(inst + 0x1C);
        if (!cd) return 0;

        uintptr_t vecFirst = *reinterpret_cast<uintptr_t*>(cd + 0x14);
        uintptr_t vecLast  = *reinterpret_cast<uintptr_t*>(cd + 0x18);
        if (!vecFirst || vecLast <= vecFirst) return 0;

        int n = static_cast<int>((vecLast - vecFirst) / 4);
        if (n > 256) n = 256;

        for (int i = 0; i < n && written < maxN; i++) {
            uintptr_t desc = *reinterpret_cast<uintptr_t*>(vecFirst + i * 4);
            if (!desc) continue;

            uintptr_t nameStr = *reinterpret_cast<uintptr_t*>(desc + 0x04);
            uintptr_t catStr  = *reinterpret_cast<uintptr_t*>(desc + 0x08);
            uintptr_t meta    = *reinterpret_cast<uintptr_t*>(desc + 0x18);
            if (!nameStr) continue;

            PropEntry& e = out[written];
            memset(&e, 0, sizeof(e));
            if (!ReadStdString(nameStr, e.name, sizeof(e.name))) continue;
            if (catStr) ReadStdString(catStr, e.category, sizeof(e.category));

            if (meta) {
                uintptr_t getter = *reinterpret_cast<uintptr_t*>(meta + 0x08);
                DecodePropGetter(getter, e.kind, e.offset, e.offset2);
            }
            written++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return written; }
    return written;
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
