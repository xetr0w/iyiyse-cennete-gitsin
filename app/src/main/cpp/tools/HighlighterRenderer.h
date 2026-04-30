#pragma once

#include "IToolRenderer.h"
#include <vector>
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRect.h"

/**
 * Fosforlu kalem (highlighter) renderer'ı.
 *
 * RENDER ÖZELLİKLERİ:
 *   - Blend mode  : Multiply (yalnızca endStroke'ta)
 *   - Genişlik    : pressure'a DUYARSIZ (sabit)
 *   - Opacity     : sabit %50
 *   - Anti-alias  : aktif
 *   - Stroke cap  : Square
 *
 * SIFIR GECİKME + DOĞRU BLEND — saveLayer Mimarisi:
 *   beginStroke → canvas->saveLayer(bounds, nullptr)
 *   addPoints → saveLayer canvas'ına SrcOver ile çiz.
 *   endStroke → canvas->restore()
 */
class HighlighterRenderer final : public IToolRenderer {
public:
    HighlighterRenderer()  = default;
    ~HighlighterRenderer() override = default;

    void beginStroke    (const StrokeParams&             params) override;
    void addPoints      (SkCanvas*                       canvas,
                         const std::vector<RenderPoint>& points) override;
    void endStroke      (SkCanvas*                       canvas) override;
    void drawGhostPoints(SkCanvas*                       canvas,
                         const std::vector<RenderPoint>& points) override;

private:
    /**
     * saveLayer açık mı?
     * endStroke'ta restore() çağrılmadan önce kontrol edilir.
     */
    bool layerSaved_ = false;

    /**
     * saveLayer için bounds rect (surfaceWidth x surfaceHeight).
     * beginStroke'ta StrokeParams'tan hesaplanır.
     */
    SkRect layerBounds_;

    /**
     * saveLayer canvas'ına çizim için paint.
     * Style: kStroke, Cap: kSquare, BlendMode: kSrcOver.
     * beginStroke'ta hazırlanır.
     */
    SkPaint offscreenPaint_;

    /**
     * saveLayer canvas'ına noktaları çizer (SrcOver).
     * addPoints ve drawGhostPoints tarafından kullanılır.
     *
     * @param canvas   saveLayer'ın canvas'ı.
     * @param points   Çizilecek noktalar.
     * @param opacity  0.0f – 1.0f alpha.
     */
    void drawToLayer(SkCanvas*                       canvas,
                     const std::vector<RenderPoint>& points,
                     float                           opacity);
};
