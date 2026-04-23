#include "properties.h"
#include "roblox.h"
#include "vendor/imgui/imgui.h"
#include <cstdio>
#include <cstring>

namespace properties {

static char s_className[64] = {};
static PropEntry s_props[256];
static int       s_propCount = 0;
static int       s_decoded   = 0;
static uint32_t  s_lastInst  = 0;

// ── BrickColor name lookup ────────────────────────────────────────────────────
// `switch` lets the compiler pick a jump table or binary-search decision tree.
static const char* BrickColorName(int id) {
    switch (id) {
        case 1:   return "White";
        case 2:   return "Grey";
        case 3:   return "Light yellow";
        case 5:   return "Brick yellow";
        case 6:   return "Light green";
        case 9:   return "Light reddish violet";
        case 11:  return "Pastel Blue";
        case 12:  return "Light orange brown";
        case 18:  return "Nougat";
        case 21:  return "Bright red";
        case 22:  return "Med. reddish violet";
        case 23:  return "Bright blue";
        case 24:  return "Yellow";
        case 25:  return "Earth orange";
        case 26:  return "Black";
        case 27:  return "Dark grey";
        case 28:  return "Dark green";
        case 29:  return "Med. stone grey";
        case 36:  return "Br. yel. orange";
        case 37:  return "Bright green";
        case 38:  return "Dark orange";
        case 40:  return "Sand blue";
        case 41:  return "Sand violet";
        case 42:  return "Peach";
        case 43:  return "Cobalt";
        case 44:  return "Slime green";
        case 45:  return "Smoky grey";
        case 47:  return "Gold";
        case 48:  return "Dark curry";
        case 49:  return "Fire yellow";
        case 50:  return "Flame yel. orange";
        case 100: return "Light orange";
        case 101: return "Bright orange";
        case 102: return "Bright bl.-green";
        case 103: return "Earth yellow";
        case 104: return "Bright violet";
        case 106: return "Br. orange";
        case 107: return "Bright bl. green";
        case 119: return "Lime green";
        case 120: return "Lt. yel. green";
        case 125: return "Light orange";
        case 151: return "Sand green";
        case 192: return "Reddish brown";
        case 194: return "Med. stone grey";
        case 195: return "Smoky grey";
        case 199: return "Dark blue";
        case 208: return "Dark yellow";
        case 217: return "Sand orange";
        case 226: return "Cool yellow";
        default:  return nullptr;
    }
}

// ── Value reader ─────────────────────────────────────────────────────────────
static const char* ReadValue(uint32_t inst, const PropEntry& e, char* buf, size_t sz) {
    buf[0] = '\0';
    __try {
        switch (e.kind) {

        case PK_FLOAT: {
            float v = *reinterpret_cast<float*>(inst + e.offset);
            snprintf(buf, sz, "%.4g", v);
            break;
        }
        case PK_INT32: {
            int32_t v = *reinterpret_cast<int32_t*>(inst + e.offset);
            if (e.flags & PF_COLORNAME) {
                const char* nm = BrickColorName(v);
                if (nm) snprintf(buf, sz, "%s (%d)", nm, v);
                else    snprintf(buf, sz, "BrickColor (%d)", v);
            } else {
                snprintf(buf, sz, "%d", v);
            }
            break;
        }
        case PK_BOOL: {
            uint8_t v = *reinterpret_cast<uint8_t*>(inst + e.offset);
            return v ? "true" : "false";
        }
        case PK_BITFIELD_BOOL: {
            uint8_t v = *reinterpret_cast<uint8_t*>(inst + e.offset);
            return (v & e.offset2) ? "true" : "false";
        }
        case PK_VECTOR3: {
            float* v = reinterpret_cast<float*>(inst + e.offset);
            snprintf(buf, sz, "%.3g, %.3g, %.3g", v[0], v[1], v[2]);
            break;
        }
        case PK_STRING:
            ReadStdString(inst + e.offset, buf, sz);
            break;
        case PK_CONST_BOOL:
            return e.offset ? "true" : "false";

        case PK_INDIRECT_FLOAT: {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(inst + e.offset);
            if (!ptr) return "nil";
            float v = *reinterpret_cast<float*>(ptr + e.offset2);
            snprintf(buf, sz, "%.4g", v);
            break;
        }
        case PK_INDIRECT_INT32: {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(inst + e.offset);
            if (!ptr) return "nil";
            int32_t v = *reinterpret_cast<int32_t*>(ptr + e.offset2);
            snprintf(buf, sz, "%d", v);
            break;
        }
        case PK_INDIRECT_BOOL: {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(inst + e.offset);
            if (!ptr) return "nil";
            uint8_t v = *reinterpret_cast<uint8_t*>(ptr + e.offset2);
            return v ? "true" : "false";
        }
        case PK_INDIRECT_VECTOR3: {
            uintptr_t ptr = *reinterpret_cast<uintptr_t*>(inst + e.offset);
            if (!ptr) return "nil";
            float* v = reinterpret_cast<float*>(ptr + e.offset2);
            snprintf(buf, sz, "%.3g, %.3g, %.3g", v[0], v[1], v[2]);
            break;
        }

        case PK_DEREF_VECTOR3:
        case PK_DEREF_VECTOR3_P4: {
            // body = *(inst+off1), vecPtr = *(body+off2), Vec3 at vecPtr+4.
            // P4 is the FPU-copy variant of the same chain — identical read path.
            uintptr_t body = *reinterpret_cast<uintptr_t*>(inst + e.offset);
            if (!body) return "nil";
            uintptr_t vecPtr = *reinterpret_cast<uintptr_t*>(body + e.offset2);
            if (!vecPtr) return "nil";
            float* v = reinterpret_cast<float*>(vecPtr + 4);
            snprintf(buf, sz, "%.3g, %.3g, %.3g", v[0], v[1], v[2]);
            break;
        }

        case PK_POSITION: {
            // World position via the known prim chain: *(*(inst+0x24)+0x164)
            uintptr_t prim = *reinterpret_cast<uintptr_t*>(inst + 0x24);
            if (!prim) return "nil";
            float* v = reinterpret_cast<float*>(prim + 0x164);
            snprintf(buf, sz, "%.3g, %.3g, %.3g", v[0], v[1], v[2]);
            break;
        }

        case PK_VTABLE_RELAY:
            // Relays are resolved once per instance-change in Draw() and rewritten to
            // their real kind, so this branch only fires if resolution failed.
            break;

        default:
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(buf, sz, "err");
    }
    return buf;
}

// ── Draw ─────────────────────────────────────────────────────────────────────
void Draw(uint32_t inst) {
    if (!inst) {
        ImGui::TextDisabled("No selection");
        return;
    }

    if (inst != s_lastInst) {
        s_lastInst  = inst;
        s_propCount = EnumerateProperties(inst, s_props, 256);
        s_decoded   = 0;
        // One-shot per-instance work: resolve vtable relays via the live vtable, and
        // pre-flag int32 props whose name signals BrickColor lookup. Avoids redoing
        // either of these on every frame inside ReadValue.
        uintptr_t vtbl = SafeDeref<uintptr_t>(inst, 0);
        for (int i = 0; i < s_propCount; i++) {
            PropEntry& pe = s_props[i];
            if (pe.kind == PK_VTABLE_RELAY && vtbl) {
                uintptr_t fn = SafeDeref<uintptr_t>(vtbl + pe.offset, 0);
                uint8_t kind = PK_UNKNOWN; uint32_t o1 = 0, o2 = 0;
                if (fn && DecodePropGetter(fn, kind, o1, o2) && kind != PK_VTABLE_RELAY) {
                    pe.kind = kind; pe.offset = o1; pe.offset2 = o2;
                } else {
                    pe.kind = PK_UNKNOWN;
                }
            }
            if (pe.kind == PK_INT32 &&
                (strstr(pe.name, "Color") || strstr(pe.name, "colour")))
                pe.flags |= PF_COLORNAME;
            if (pe.kind != PK_UNKNOWN) s_decoded++;
        }
        if (!ReadClassName(inst, s_className, sizeof(s_className)))
            snprintf(s_className, sizeof(s_className), "Instance");
    }

    ImGui::TextUnformatted(s_className);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d / %d)", s_decoded, s_propCount);
    ImGui::Separator();

    if (s_decoded == 0) {
        ImGui::TextDisabled("No readable properties");
        return;
    }

    ImGui::BeginChild("##props_scroll", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    if (ImGui::BeginTable("##props", 2,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingStretchProp)) {

        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Value",    ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableHeadersRow();

        const char* lastCat = nullptr;
        char vbuf[128];

        for (int i = 0; i < s_propCount; i++) {
            const PropEntry& e = s_props[i];
            if (e.kind == PK_UNKNOWN) continue;

            if (e.category[0] && (lastCat == nullptr || strcmp(e.category, lastCat) != 0)) {
                lastCat = e.category;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 100, 255));
                ImGui::TextUnformatted(e.category);
                ImGui::PopStyleColor();
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(e.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(ReadValue(inst, e, vbuf, sizeof(vbuf)));
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

} // namespace properties
