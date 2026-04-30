#pragma once

#include "IToolRenderer.h"
#include <vector>
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"

/**
 * Silgi renderer'ı.
 *
 * RENDER ÖZELLİKLERİ:
 *   - Blend mode  : Clear (hedef pikselleri siler)
 *   - Genişlik    : pressure'a duyarsız (sabit genişlik veya baseWidth x çarpan)
 *   - Anti-alias  : aktif
 *   - Stroke cap  : Round
 *
 * GÖRSEL SİLME:
 *   Kullanıcı silgiyle dokunduğunda kClear moduyla canvas üzerindeki
 *   ilgili pikseller tamamen şeffaf hale getirilir.
 *   (Bu işlem görsel bir illüzyondur, gerçek model silme işlemi
 *   ACTION_UP sonrasında Kotlin tarafında DeleteStrokeCommand ile yapılır).
 */
class EraserRenderer final : public IToolRenderer {
public:
    EraserRenderer()  = default;
    ~EraserRenderer() override = default;

    void beginStroke    (const StrokeParams&             params) override;
    void addPoints      (SkCanvas*                       canvas,
                         const std::vector<RenderPoint>& points) override;
    void endStroke      (SkCanvas*                       canvas) override;
    void drawGhostPoints(SkCanvas*                       canvas,
                         const std::vector<RenderPoint>& points) override;

private:
    /**
     * Silgi paint'i. BlendMode: kClear.
     */
    SkPaint eraserPaint_;

    /**
     * Geçici çizimde (ghost) nereyi sildiğimizi göstermek için paint.
     */
    SkPaint ghostEraserPaint_;
};
