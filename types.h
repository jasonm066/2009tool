#pragma once
#include <cstdint>

// Core data types
struct Vec3  { float x, y, z; };
struct Color { float r, g, b, a; };
struct Camera {
    float rot[9];   // 3x3 row-major
    Vec3  pos;
    float fov;      // radians
};
