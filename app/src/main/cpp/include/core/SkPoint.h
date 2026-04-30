#pragma once
struct SkPoint {
    float fX, fY;
    static SkPoint Make(float x, float y) { return {x, y}; }
};
