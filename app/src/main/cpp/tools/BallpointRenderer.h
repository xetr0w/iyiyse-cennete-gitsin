#pragma once

#include "IToolRenderer.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkVertices.h"
#include <vector>

/**
 * Tükenmez kalem renderer'ı.
 *
 * RENDER ÖZELLİKLERİ:
 *   - Blend mode  : SrcOver
 *   - Genişlik    : pressure-sensitive
 *   - Anti-alias  : aktif
 *   - Join        : Miter Join (limit aşılınca Bevel)
 *
 * MESH ALGORİTMASI (Triangle Strip):
 *   Her Catmull-Rom noktası için teğet vektörü hesaplanır.
 *   Teğete dik normal ile sol/sağ vertex çifti üretilir.
 *   Segment birleşimlerinde gerçek Miter Join uygulanır:
 *     N = normalize(T1 + T2)
 *     P ± N × (w / (2 × cos(θ/2)))
 *   Açı limiti (kMiterLimit) aşılırsa Bevel Join'e geçilir.
 *   Tüm vertex'ler SkVertices (kTriangleStrip_VertexMode) ile
 *   canvas->drawVertices() üzerinden GPU'ya gönderilir.
 */
class BallpointRenderer final : public IToolRenderer {
public:
    BallpointRenderer()  = default;
    ~BallpointRenderer() override = default;

    void beginStroke    (const StrokeParams&             params)          override;
    void addPoints      (SkCanvas*                       canvas,
                         const std::vector<RenderPoint>& points)          override;
    void endStroke      (SkCanvas*                       canvas)          override;
    void drawGhostPoints(SkCanvas*                       canvas,
                         const std::vector<RenderPoint>& points)          override;

private:
    // ── Mesh buffer ───────────────────────────────────────────────────────────
    /**
     * Triangle Strip vertex'leri (x,y çiftleri).
     * Format: [leftX0, leftY0, rightX0, rightY0, leftX1, rightY1, ...]
     * flushMesh() sonrası temizlenir.
     */
    std::vector<float> meshVertices_;

    // ── Miter Join için önceki frame verisi ──────────────────────────────────
    float prevTx_      = 0.0f;   // önceki segment teğeti X
    float prevTy_      = 0.0f;   // önceki segment teğeti Y
    float prevLeftX_   = 0.0f;
    float prevLeftY_   = 0.0f;
    float prevRightX_  = 0.0f;
    float prevRightY_  = 0.0f;
    bool  hasJoinData_ = false;

    SkPaint strokePaint_;

    /**
     * İki RenderPoint için sol/sağ vertex çifti üretir.
     * Gerçek Miter Join: N = normalize(T1+T2), P ± N×(w/(2cos(θ/2)))
     * Limit aşılınca Bevel Join.
     */
    void appendSegment(const RenderPoint& from, const RenderPoint& to);

    /**
     * meshVertices_ içeriğini SkVertices ile canvas'a çizer.
     * addPoints (incremental) ve endStroke (final flush) çağırır.
     */
    void flushMesh(SkCanvas* canvas);
};
