#pragma once
#include "vendor/imgui/imgui.h"
#include "types.h"
#include <cmath>

// ============================================================
//  Drawing primitives — render onto ImGui's background draw list.
//  Identical semantics to the external ESP renderer.h, minus the
//  D3D9 lifecycle (the host hooks handle that).
// ============================================================

inline ImDrawList* Overlay() { return ImGui::GetBackgroundDrawList(); }

inline ImU32 ToU32(Color c) {
    return IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), (int)(c.a * 255));
}

inline void DrawLine(float x1, float y1, float x2, float y2, float t, Color c) {
    Overlay()->AddLine({x1, y1}, {x2, y2}, ToU32(c), t);
}
inline void DrawRect(float x, float y, float w, float h, float t, Color c) {
    Overlay()->AddRect({x, y}, {x + w, y + h}, ToU32(c), 0, 0, t);
}
inline void DrawRectFilled(float x, float y, float w, float h, Color c) {
    Overlay()->AddRectFilled({x, y}, {x + w, y + h}, ToU32(c));
}
inline void DrawCornerBox(float x, float y, float w, float h, float t, Color c) {
    auto* dl = Overlay(); ImU32 u = ToU32(c);
    float L = (w < h ? w : h) * 0.25f;
    dl->AddLine({x,     y    }, {x + L, y    }, u, t);
    dl->AddLine({x,     y    }, {x,     y + L}, u, t);
    dl->AddLine({x + w, y    }, {x + w - L, y}, u, t);
    dl->AddLine({x + w, y    }, {x + w, y + L}, u, t);
    dl->AddLine({x,     y + h}, {x + L, y + h}, u, t);
    dl->AddLine({x,     y + h}, {x,     y + h - L}, u, t);
    dl->AddLine({x + w, y + h}, {x + w - L, y + h}, u, t);
    dl->AddLine({x + w, y + h}, {x + w, y + h - L}, u, t);
}
inline void DrawGradientV(float x, float y, float w, float h, Color top, Color bot) {
    Overlay()->AddRectFilledMultiColor({x, y}, {x + w, y + h},
                                       ToU32(top), ToU32(top), ToU32(bot), ToU32(bot));
}
inline void DrawGradientOutline(float x, float y, float w, float h, float t, Color top, Color bot) {
    auto* dl = Overlay(); ImU32 cT = ToU32(top), cB = ToU32(bot);
    dl->AddRectFilledMultiColor({x, y},         {x + t, y + h}, cT, cT, cB, cB);
    dl->AddRectFilledMultiColor({x + w - t, y}, {x + w, y + h}, cT, cT, cB, cB);
    dl->AddRectFilled({x, y},         {x + w, y + t}, cT);
    dl->AddRectFilled({x, y + h - t}, {x + w, y + h}, cB);
}
inline void DrawOutlinedText(float x, float y, const char* text, Color c) {
    auto* dl = Overlay();
    ImU32 sh = IM_COL32(0, 0, 0, 255);
    dl->AddText({x - 1, y}, sh, text);
    dl->AddText({x + 1, y}, sh, text);
    dl->AddText({x, y - 1}, sh, text);
    dl->AddText({x, y + 1}, sh, text);
    dl->AddText({x, y}, ToU32(c), text);
}
inline void DrawTriangleFilled(float x1, float y1, float x2, float y2,
                               float x3, float y3, Color c) {
    Overlay()->AddTriangleFilled({x1, y1}, {x2, y2}, {x3, y3}, ToU32(c));
}

// World-to-screen
inline bool WorldToScreen(const Camera& cam, const Vec3& w, int sw, int sh,
                          float& sx, float& sy) {
    float dx = w.x - cam.pos.x;
    float dy = w.y - cam.pos.y;
    float dz = w.z - cam.pos.z;
    float lx = cam.rot[0] * dx + cam.rot[3] * dy + cam.rot[6] * dz;
    float ly = cam.rot[1] * dx + cam.rot[4] * dy + cam.rot[7] * dz;
    float lz = cam.rot[2] * dx + cam.rot[5] * dy + cam.rot[8] * dz;
    if (lz >= 0.f) return false;
    float focal = (sh * 0.5f) / tanf(cam.fov * 0.5f);
    sx = (lx / -lz) * focal + sw * 0.5f;
    sy = (-ly / -lz) * focal + sh * 0.5f;
    return true;
}
