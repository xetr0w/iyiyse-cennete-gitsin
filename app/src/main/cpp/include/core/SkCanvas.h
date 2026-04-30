#pragma once
#include "SkMatrix.h"
#include "SkImage.h"
#include "SkPaint.h"
#include "SkVertices.h"
#include "SkPath.h"
#include "SkRect.h"
#define SK_ColorTRANSPARENT 0x00000000
class SkCanvas {
public:
    void clear(unsigned int color) {}
    void resetMatrix() {}
    void setMatrix(const SkMatrix& matrix) {}
    void drawImage(sk_sp<SkImage> image, float x, float y) {}
    void drawVertices(sk_sp<SkVertices>, SkBlendMode, const SkPaint&) {}
    void drawPath(const SkPath&, const SkPaint&) {}
    void saveLayer(const SkRect*, const SkPaint*) {}
    void restore() {}
};
