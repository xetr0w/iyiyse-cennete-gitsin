#pragma once

#include <vector>
#include <cstdint>
#include "include/core/SkCanvas.h"
#include "include/core/SkRefCnt.h"

// JNI'dan gelen ham noktalar için yapı
struct RenderPoint {
    float x;
    float y;
    float pressure;
    int64_t timestamp; // nanosecond
};

// Renderer başlatma parametreleri
struct StrokeParams {
    int color;
    float baseWidth;
    int hardwareHz;
    int surfaceWidth;
    int surfaceHeight;
};

// Tüm çizim araçlarının (Kalem, Fosforlu vb.) türemesi gereken arayüz
class IToolRenderer : public SkRefCnt {
public:
    virtual ~IToolRenderer() = default;

    // Stroke başlatma (Renk, kalınlık vb. ayarlanır)
    virtual void beginStroke(const StrokeParams& params) = 0;

    // Kalıcı çizgi için yeni noktalar eklendiğinde çağrılır
    virtual void addPoints(SkCanvas* canvas, const std::vector<RenderPoint>& points) = 0;

    // Stroke bittiğinde çağrılır (Örn: Highlighter için saveLayer kapatılır)
    virtual void endStroke(SkCanvas* canvas) = 0;

    // Ghost noktaları ekrana çizmek için çağrılır (Kalıcı yüzeye dokunmaz)
    virtual void drawGhostPoints(SkCanvas* canvas, const std::vector<RenderPoint>& points) = 0;

protected:
    StrokeParams activeParams_{};

    static float pressureToWidth(float pressure, float baseWidth) {
        constexpr float kMinFactor = 0.3f;
        constexpr float kMaxFactor = 1.0f;
        const float clamped = (pressure < 0.0f) ? 0.0f
                            : (pressure > 1.0f) ? 1.0f
                            : pressure;
        return baseWidth * (kMinFactor + clamped * (kMaxFactor - kMinFactor));
    }
};
