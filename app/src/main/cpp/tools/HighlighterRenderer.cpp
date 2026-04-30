#include "HighlighterRenderer.h"
#include <android/log.h>

#define LOG_TAG "HighlighterRenderer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
    constexpr float kHighlighterOpacity = 0.50f;
    constexpr float kGhostOpacity       = 0.25f;
    constexpr float kWidthMultiplier    = 3.0f;
}

// ─────────────────────────────────────────────────────────────────────────────

void HighlighterRenderer::beginStroke(const StrokeParams& params) {
    activeParams_ = params;
    layerSaved_   = false;

    LOGD("HighlighterRenderer::beginStroke color=0x%08X width=%.2f hz=%d "
         "surface=%dx%d",
         params.color, params.baseWidth, params.hardwareHz,
         params.surfaceWidth, params.surfaceHeight);

    layerBounds_ = SkRect::MakeWH(
        static_cast<float>(params.surfaceWidth),
        static_cast<float>(params.surfaceHeight)
    );

    offscreenPaint_.reset();
    offscreenPaint_.setAntiAlias(true);
    offscreenPaint_.setStyle(SkPaint::kStroke_Style);
    offscreenPaint_.setBlendMode(SkBlendMode::kSrcOver);
    offscreenPaint_.setStrokeWidth(params.baseWidth * kWidthMultiplier);
    offscreenPaint_.setStrokeCap(SkPaint::kSquare_Cap);
    offscreenPaint_.setAlphaf(kHighlighterOpacity);
    offscreenPaint_.setColor(params.color);
}

// ─────────────────────────────────────────────────────────────────────────────

void HighlighterRenderer::addPoints(SkCanvas*                       canvas,
                                    const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr) {
        LOGE("HighlighterRenderer::addPoints: canvas is null.");
        return;
    }
    if (points.empty()) {
        return;
    }

    // Lazy saveLayer
    if (!layerSaved_) {
        SkPaint layerPaint;
        layerPaint.setBlendMode(SkBlendMode::kMultiply);
        canvas->saveLayer(&layerBounds_, &layerPaint);
        layerSaved_ = true;
    }

    drawToLayer(canvas, points, kHighlighterOpacity);

    LOGD("HighlighterRenderer::addPoints: %zu points to layer.", points.size());
}

// ─────────────────────────────────────────────────────────────────────────────

void HighlighterRenderer::endStroke(SkCanvas* canvas) {
    if (canvas == nullptr) {
        LOGE("HighlighterRenderer::endStroke: canvas is null.");
        layerSaved_ = false;
        return;
    }

    if (layerSaved_) {
        canvas->restore();
        layerSaved_ = false;
    }

    LOGD("HighlighterRenderer::endStroke: layer composited with kMultiply.");
}

// ─────────────────────────────────────────────────────────────────────────────

void HighlighterRenderer::drawGhostPoints(SkCanvas*                       canvas,
                                          const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr || points.empty()) {
        return;
    }

    SkPaint ghostLayerPaint;
    ghostLayerPaint.setBlendMode(SkBlendMode::kSrcOver);
    canvas->saveLayer(&layerBounds_, &ghostLayerPaint);
    drawToLayer(canvas, points, kGhostOpacity);
    canvas->restore();

    LOGD("HighlighterRenderer::drawGhostPoints: %zu ghost points.", points.size());
}

// ─────────────────────────────────────────────────────────────────────────────

void HighlighterRenderer::drawToLayer(SkCanvas*                       canvas,
                                      const std::vector<RenderPoint>& points,
                                      float                           opacity)
{
    if (points.empty()) return;

    SkPath path;
    path.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); ++i) {
        path.lineTo(points[i].x, points[i].y);
    }

    SkPaint paint = offscreenPaint_;
    paint.setAlphaf(opacity);
    canvas->drawPath(path, paint);

    LOGD("HighlighterRenderer::drawToLayer: %zu points.", points.size());
}
