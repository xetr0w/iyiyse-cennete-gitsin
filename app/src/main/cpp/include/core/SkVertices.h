#pragma once
#include "SkRefCnt.h"
#include "SkPoint.h"
class SkVertices : public SkRefCnt {
public:
    enum VertexMode { kTriangleStrip_VertexMode };
    static sk_sp<SkVertices> MakeCopy(VertexMode, int, const SkPoint*, const SkPoint*, const uint32_t*) {
        return sk_make_sp<SkVertices>();
    }
};
