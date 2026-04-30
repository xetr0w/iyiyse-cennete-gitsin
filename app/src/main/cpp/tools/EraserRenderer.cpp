#include "EraserRenderer.h"
#include <android/log.h>

#define LOG_TAG "EraserRenderer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
    // Silgi genişliği biraz daha kalın olabilir (daha kolay silmek için)
    constexpr float kEraserMultiplier = 4.0f;
}

// ─────────────────────────────────────────────────────────────────────────────

void EraserRenderer::beginStroke(const StrokeParams& params) {
    activeParams_ = params;

    LOGD("EraserRenderer::beginStroke width=%.2f hz=%d",
         params.baseWidth, params.hardwareHz);

    // KClear blend mode ile çizilen yerdeki pikselleri temizler
    eraserPaint_.reset();
    eraserPaint_.setAntiAlias(true);
    eraserPaint_.setStyle(SkPaint::kStroke_Style);
    eraserPaint_.setBlendMode(SkBlendMode::kClear);
    eraserPaint_.setStrokeWidth(params.baseWidth * kEraserMultiplier);
    eraserPaint_.setStrokeCap(SkPaint::kRound_Cap);
    eraserPaint_.setStrokeJoin(SkPaint::kRound_Join);

    // Ghost renderer için yarı saydam beyaz/gri bir gösterge
    ghostEraserPaint_.reset();
    ghostEraserPaint_.setAntiAlias(true);
    ghostEraserPaint_.setStyle(SkPaint::kStroke_Style);
    ghostEraserPaint_.setBlendMode(SkBlendMode::kSrcOver);
    ghostEraserPaint_.setStrokeWidth(params.baseWidth * kEraserMultiplier);
    ghostEraserPaint_.setStrokeCap(SkPaint::kRound_Cap);
    ghostEraserPaint_.setStrokeJoin(SkPaint::kRound_Join);
    ghostEraserPaint_.setColor(0x80FFFFFF); // %50 saydam beyaz
}

// ─────────────────────────────────────────────────────────────────────────────

void EraserRenderer::addPoints(SkCanvas*                       canvas,
                               const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr) {
        LOGE("EraserRenderer::addPoints: canvas is null.");
        return;
    }
    if (points.empty()) {
        return;
    }

    SkPath path;
    path.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); ++i) {
        path.lineTo(points[i].x, points[i].y);
    }

    canvas->drawPath(path, eraserPaint_);

    LOGD("EraserRenderer::addPoints: %zu points erased.", points.size());
}

// ─────────────────────────────────────────────────────────────────────────────

void EraserRenderer::endStroke(SkCanvas* canvas) {
    if (canvas == nullptr) {
        LOGE("EraserRenderer::endStroke: canvas is null.");
        return;
    }

    // Ekstra temizlik gerekmiyor, path anında çiziliyor
    LOGD("EraserRenderer::endStroke: done.");
}

// ─────────────────────────────────────────────────────────────────────────────

void EraserRenderer::drawGhostPoints(SkCanvas*                       canvas,
                                     const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr || points.empty()) {
        return;
    }

    SkPath path;
    path.moveTo(points[0].x, points[0].y);
    for (size_t i = 1; i < points.size(); ++i) {
        path.lineTo(points[i].x, points[i].y);
    }

    // Ghost silgi göstergesi ana silgiye kClear uygulamaz
    // Nereyi sildiğimizi göstermek için bir ipucu çizeriz
    canvas->drawPath(path, ghostEraserPaint_);

    LOGD("EraserRenderer::drawGhostPoints: %zu ghost points.", points.size());
}
