#pragma once
struct SkRect {
    float fLeft, fTop, fRight, fBottom;
    static SkRect MakeWH(float w, float h) { return {0, 0, w, h}; }
};
