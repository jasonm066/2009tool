#pragma once
#include "types.h"
#include <cstdio>
#include <cstdint>
#include <cstddef>

// ============================================================
//  ESP + executor config (saved to esp_config.dat next to game)
// ============================================================
static const char*  CONFIG_FILE    = "esp_config.dat";
static const uint32_t CONFIG_VERSION = 2;

struct Config {
    // ESP
    bool box       = true;
    bool health    = true;
    bool name      = true;
    bool snaplines = false;
    bool distance  = false;
    bool offscreen = false;
    float arrowScale = 12.f;

    int   boxStyle    = 0;
    bool  boxGradient = false;
    bool  boxFilled   = false;
    float fillAlpha   = 0.25f;

    float colBox [3]  = {1.0f, 0.2f, 0.2f};
    float colBox2[3]  = {0.2f, 0.2f, 1.0f};
    bool  hpGradient  = false;
    float colHP1 [3]  = {0.2f, 1.0f, 0.2f};
    float colHP2 [3]  = {1.0f, 0.2f, 0.2f};
    float colName[3]  = {1.0f, 1.0f, 1.0f};
    float colSnap[3]  = {1.0f, 1.0f, 0.2f};
    float colDist[3]  = {0.8f, 0.8f, 0.8f};
    float colOfs [3]  = {1.0f, 0.5f, 0.0f};

    // Runtime (not persisted; comes after this marker)
    bool showMenu = false;

    Color boxColor()   const { return {colBox [0], colBox [1], colBox [2], 1.f}; }
    Color boxColor2()  const { return {colBox2[0], colBox2[1], colBox2[2], 1.f}; }
    Color nameColor()  const { return {colName[0], colName[1], colName[2], 1.f}; }
    Color snapColor()  const { return {colSnap[0], colSnap[1], colSnap[2], 1.f}; }
    Color distColor()  const { return {colDist[0], colDist[1], colDist[2], 1.f}; }
    Color ofsColor()   const { return {colOfs [0], colOfs [1], colOfs [2], 1.f}; }
    Color hpFull()     const { return {colHP1 [0], colHP1 [1], colHP1 [2], 1.f}; }
    Color hpEmpty()    const { return {colHP2 [0], colHP2 [1], colHP2 [2], 1.f}; }
    Color fillColor()  const { return {colBox [0], colBox [1], colBox [2], fillAlpha}; }

    bool Save() const {
        FILE* f = fopen(CONFIG_FILE, "wb"); if (!f) return false;
        fwrite(&CONFIG_VERSION, sizeof(uint32_t), 1, f);
        size_t n = offsetof(Config, showMenu) - offsetof(Config, box);
        fwrite(&box, 1, n, f);
        fclose(f); return true;
    }
    bool Load() {
        FILE* f = fopen(CONFIG_FILE, "rb"); if (!f) return false;
        uint32_t v = 0;
        if (fread(&v, sizeof(uint32_t), 1, f) != 1 || v != CONFIG_VERSION) { fclose(f); return false; }
        size_t n = offsetof(Config, showMenu) - offsetof(Config, box);
        bool ok = fread(&box, 1, n, f) == n;
        fclose(f); return ok;
    }
};

static const char* BOX_STYLES[] = { "Full", "Corner", "3D" };
static const int   BOX_STYLE_COUNT = 3;
