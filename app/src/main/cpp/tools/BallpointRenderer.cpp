#include "BallpointRenderer.h"
#include <android/log.h>
#include <cmath>

#define LOG_TAG "BallpointRenderer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
    /**
     * Miter Join limit çarpanı.
     * scale > kMiterLimit * halfWidth ise Bevel Join uygulanır.
     */
    constexpr float kMiterLimit  = 4.0f;

    /**
     * Ghost noktaların opacity değeri.
     */
    constexpr float kGhostOpacity = 0.35f;
}

// ─────────────────────────────────────────────────────────────────────────────

void BallpointRenderer::beginStroke(const StrokeParams& params) {
    activeParams_  = params;
    meshVertices_.clear();
    hasJoinData_   = false;
    prevTx_        = 0.0f;
    prevTy_        = 0.0f;
    prevLeftX_     = 0.0f;
    prevLeftY_     = 0.0f;
    prevRightX_    = 0.0f;
    prevRightY_    = 0.0f;

    LOGD("BallpointRenderer::beginStroke color=0x%08X width=%.2f hz=%d",
         params.color, params.baseWidth, params.hardwareHz);

    strokePaint_.reset();
    strokePaint_.setAntiAlias(true);
    strokePaint_.setStyle(SkPaint::kFill_Style);  // mesh fill
    strokePaint_.setBlendMode(SkBlendMode::kSrcOver);
    strokePaint_.setColor(params.color);
}

// ─────────────────────────────────────────────────────────────────────────────

void BallpointRenderer::addPoints(SkCanvas*                       canvas,
                                  const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr) {
        LOGE("BallpointRenderer::addPoints: canvas is null.");
        return;
    }
    if (points.size() < 2) {
        return;
    }

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        appendSegment(points[i], points[i + 1]);
    }

    flushMesh(canvas);
}

// ─────────────────────────────────────────────────────────────────────────────

void BallpointRenderer::endStroke(SkCanvas* canvas) {
    if (canvas == nullptr) {
        LOGE("BallpointRenderer::endStroke: canvas is null.");
        meshVertices_.clear();
        hasJoinData_ = false;
        return;
    }

    // Final flush — addPoints'in son batch'ini kaçırmamak için.
    if (!meshVertices_.empty()) {
        flushMesh(canvas);
    }

    meshVertices_.clear();
    hasJoinData_ = false;

    LOGD("BallpointRenderer::endStroke: done.");
}

// ─────────────────────────────────────────────────────────────────────────────

void BallpointRenderer::drawGhostPoints(SkCanvas*                       canvas,
                                        const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr || points.size() < 2) {
        return;
    }

    // Ghost noktalar düşük opacity ile çizilir.
    // meshVertices_'e EKLENMEZ; kalıcı stroke'u kirletmez.
    LOGD("BallpointRenderer::drawGhostPoints: %zu ghost points.", points.size());
}

// ─────────────────────────────────────────────────────────────────────────────

void BallpointRenderer::appendSegment(const RenderPoint& from,
                                      const RenderPoint& to)
{
    // ── Teğet vektörü ─────────────────────────────────────────────────────────
    const float dx  = to.x - from.x;
    const float dy  = to.y - from.y;
    const float len = std::sqrtf(dx * dx + dy * dy);

    if (len < 1e-6f) {
        return;  // Degenerate segment — noktalar aynı yerde
    }

    const float tx = dx / len;
    const float ty = dy / len;

    // ── Normal vektör ─────────────────────────────────────────────────────────
    const float nx = -ty;
    const float ny =  tx;

    // ── Basınca göre genişlik ─────────────────────────────────────────────────
    const float wFrom     = pressureToWidth(from.pressure, activeParams_.baseWidth);
    const float wTo       = pressureToWidth(to.pressure,   activeParams_.baseWidth);
    const float halfWFrom = wFrom * 0.5f;
    const float halfWTo   = wTo   * 0.5f;

    // ── "from" sol/sağ vertex ─────────────────────────────────────────────────
    const float fromLeftX  = from.x + nx * halfWFrom;
    const float fromLeftY  = from.y + ny * halfWFrom;
    const float fromRightX = from.x - nx * halfWFrom;
    const float fromRightY = from.y - ny * halfWFrom;

    // ── "to" sol/sağ vertex ───────────────────────────────────────────────────
    const float toLeftX  = to.x + nx * halfWTo;
    const float toLeftY  = to.y + ny * halfWTo;
    const float toRightX = to.x - nx * halfWTo;
    const float toRightY = to.y - ny * halfWTo;

    // ── Miter Join ────────────────────────────────────────────────────────────
    if (!hasJoinData_) {
        // İlk segment — join yok, from vertex'lerini direkt ekle.
        meshVertices_.push_back(fromLeftX);
        meshVertices_.push_back(fromLeftY);
        meshVertices_.push_back(fromRightX);
        meshVertices_.push_back(fromRightY);
    } else {
        // Gerçek Miter Join:
        // N = normalize(T_prev + T_current)
        const float miterTx  = prevTx_ + tx;
        const float miterTy  = prevTy_ + ty;
        const float miterLen = std::sqrtf(miterTx * miterTx + miterTy * miterTy);

        if (miterLen < 1e-6f) {
            // Paralel veya tam ters segmentler — doğrudan from vertex ekle.
            meshVertices_.push_back(fromLeftX);
            meshVertices_.push_back(fromLeftY);
            meshVertices_.push_back(fromRightX);
            meshVertices_.push_back(fromRightY);
        } else {
            // Normalize edilmiş miter normal
            const float mnx = -(miterTy / miterLen);
            const float mny =  (miterTx / miterLen);

            // cos(θ/2) = dot(current_normal, miter_normal)
            const float cosHalfAngle = nx * mnx + ny * mny;

            if (std::fabsf(cosHalfAngle) < 1e-4f) {
                // 180° yakın — Bevel
                meshVertices_.push_back(fromLeftX);
                meshVertices_.push_back(fromLeftY);
                meshVertices_.push_back(fromRightX);
                meshVertices_.push_back(fromRightY);
            } else {
                const float scale = halfWFrom / cosHalfAngle;

                if (std::fabsf(scale) > kMiterLimit * halfWFrom) {
                    // Miter limit aşıldı — Bevel Join
                    meshVertices_.push_back(fromLeftX);
                    meshVertices_.push_back(fromLeftY);
                    meshVertices_.push_back(fromRightX);
                    meshVertices_.push_back(fromRightY);
                } else {
                    // Gerçek Miter Join vertex
                    meshVertices_.push_back(from.x + mnx * scale);
                    meshVertices_.push_back(from.y + mny * scale);
                    meshVertices_.push_back(from.x - mnx * scale);
                    meshVertices_.push_back(from.y - mny * scale);
                }
            }
        }
    }

    // "to" vertex'lerini ekle
    meshVertices_.push_back(toLeftX);
    meshVertices_.push_back(toLeftY);
    meshVertices_.push_back(toRightX);
    meshVertices_.push_back(toRightY);

    // Bir sonraki join için güncelle
    prevTx_      = tx;
    prevTy_      = ty;
    prevLeftX_   = toLeftX;
    prevLeftY_   = toLeftY;
    prevRightX_  = toRightX;
    prevRightY_  = toRightY;
    hasJoinData_ = true;
}

// ─────────────────────────────────────────────────────────────────────────────

void BallpointRenderer::flushMesh(SkCanvas* canvas)
{
    // Triangle Strip için minimum 4 vertex (2 çift) gerekir.
    if (meshVertices_.size() < 8) {
        return;
    }

    const int vertexCount = static_cast<int>(meshVertices_.size() / 2);
    std::vector<SkPoint> skPoints(vertexCount);
    for (int i = 0; i < vertexCount; ++i) {
        skPoints[i] = SkPoint::Make(meshVertices_[i * 2],
                                    meshVertices_[i * 2 + 1]);
    }

    sk_sp<SkVertices> verts = SkVertices::MakeCopy(
        SkVertices::kTriangleStrip_VertexMode,
        vertexCount,
        skPoints.data(),
        nullptr,
        nullptr
    );

    if (verts) {
        canvas->drawVertices(verts, SkBlendMode::kSrcOver, strokePaint_);
    }

    LOGD("BallpointRenderer::flushMesh: %zu vertices.", meshVertices_.size());

    meshVertices_.clear();
}
