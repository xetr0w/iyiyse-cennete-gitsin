#pragma once
#include "SkRefCnt.h"
#include "SkCanvas.h"
#include "SkImage.h"
#include "../gpu/GrDirectContext.h"
#include "../gpu/GrBackendSurface.h"
enum GrSurfaceOrigin { kBottomLeft_GrSurfaceOrigin };
enum SkColorType { kRGBA_8888_SkColorType };
namespace skgpu { enum class Budgeted { kNo, kYes }; }
class SkSurfaceProps {};
class SkImageInfo {
public:
    static SkImageInfo MakeN32Premul(int w, int h) { return SkImageInfo(); }
};
class SkSurface : public SkRefCnt {
public:
    SkCanvas* getCanvas() { return &canvas; }
    sk_sp<SkImage> makeImageSnapshot() { return sk_sp<SkImage>(new SkImage()); }
    int width() const { return 100; }
    int height() const { return 100; }
    static sk_sp<SkSurface> MakeRenderTarget(GrDirectContext* context, skgpu::Budgeted budgeted, const SkImageInfo& info) {
        return sk_sp<SkSurface>(new SkSurface());
    }
    static sk_sp<SkSurface> MakeFromBackendRenderTarget(GrDirectContext* context, const GrBackendRenderTarget& rt, GrSurfaceOrigin origin, SkColorType colorType, sk_sp<SkColorSpace> colorSpace, const SkSurfaceProps* props) {
        return sk_sp<SkSurface>(new SkSurface());
    }
private:
    SkCanvas canvas;
};
