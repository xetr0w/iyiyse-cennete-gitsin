#pragma once

#include "IToolRenderer.h"
#include "BallpointRenderer.h"
#include "HighlighterRenderer.h"
#include "EraserRenderer.h"
#include "include/core/SkRefCnt.h"

/**
 * ToolType ordinal değerine göre doğru renderer'ı oluşturur.
 *
 * Kotlin ToolType enum sırası:
 *   0 = BALLPOINT_PEN
 *   1 = FOUNTAIN_PEN
 *   2 = HIGHLIGHTER
 *   3 = MARKER
 *   4 = ERASER
 */
class ToolRendererFactory {
public:
    static sk_sp<IToolRenderer> create(int toolTypeOrd) {
        switch (toolTypeOrd) {
            case 0:  // BALLPOINT_PEN
                return sk_make_sp<BallpointRenderer>();

            case 1:  // FOUNTAIN_PEN (Henüz yok, fallback Ballpoint)
                return sk_make_sp<BallpointRenderer>();

            case 2:  // HIGHLIGHTER
                return sk_make_sp<HighlighterRenderer>();

            case 3:  // MARKER (Henüz yok, fallback Ballpoint)
                return sk_make_sp<BallpointRenderer>();

            case 4:  // ERASER
                return sk_make_sp<EraserRenderer>();

            default:
                return sk_make_sp<BallpointRenderer>();
        }
    }

    ToolRendererFactory()  = delete;
    ~ToolRendererFactory() = delete;
};
