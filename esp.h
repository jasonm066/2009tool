#pragma once
#include "config.h"
#include "types.h"

namespace esp {
// Draw all ESP elements onto the ImGui background draw list.
// `sw`/`sh` = current backbuffer dimensions.
void Draw(int sw, int sh, const Config& cfg);
}
