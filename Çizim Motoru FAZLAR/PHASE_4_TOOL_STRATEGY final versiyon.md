# FAZ 4: TOOL STRATEGY & COMMAND PATTERN
**Versiyon: 3.0 — Nihai ve Kilitli**

---

## Mimari Pozisyon

```
DynamicInputSampler (Kotlin / Faz 2) [1]
│   ACTION_DOWN / ACTION_MOVE / ACTION_UP
│   canonical koordinatlar, ham StylusPoint listesi
▼
NativeDrawingEngine.kt (Kotlin / Faz 3) [4]
│   beginStroke / addPoints / endStroke → JNI
▼
DrawingEngine (C++ / Faz 3) [4]
│   swap trick → Render Thread → Catmull-Rom
▼
ToolRendererFactory (C++ / Faz 4)
│   sk_sp<IToolRenderer>
▼
IToolRenderer → BallpointRenderer / HighlighterRenderer
▼
SkCanvas → EGL → ANativeWindow [4]

ACTION_UP (Kotlin / Faz 2) [1]
│   ham StylusPoint listesi zaten Kotlin'de
▼
CommandManager (Kotlin / Faz 4)
│   AddStrokeCommand / DeleteStrokeCommand / TransformStrokesCommand
▼
Page.layers[activeLayerIndex].strokes (PersistentList) [2]
```

---

## Katmanlar Arası İş Bölümü

**C++ tarafı** yalnızca **görselleştirmeden** sorumludur. Kalıcı veri üretmez, model değiştirmez [4].

**Kotlin tarafı** yalnızca **state yönetiminden** sorumludur. `ACTION_UP` anında kendi elindeki ham noktalarla `Stroke` objesini oluşturur, `CommandManager`'ı tetikler. C++'tan veri beklemez [1].

**JNI köprüsü tek yönlü çalışır: Kotlin → C++.** Noktalar C++'tan Kotlin'e geri yollanmaz [4].

```
Doğru akış:

ACTION_UP (Kotlin) [1]
├─► endStroke() JNI → C++ görsel mühürler, buffer temizler [4]
└─► Kotlin elindeki StylusPoint listesinden Stroke objesi oluşturur
    └─► CommandManager.execute(AddStrokeCommand(layerId, stroke))
        └─► Page.layers[activeLayer].strokes.add(stroke)
            └─► PersistentList structural sharing [2]
```

---

## Dosya 1 — Kotlin Command Pattern

**Path:** `app/src/main/java/com/notia/engine/command/ICommand.kt`

```kotlin
package com.notia.engine.command

/**
 * Tüm komutların uygulaması gereken soyut arayüz.
 *
 * KURAL: Her state değişikliği bir Command nesnesi üzerinden geçer.
 *        Page objesi dışarıdan doğrudan mutate edilemez [2].
 *
 * THREAD SAFETY:
 *   execute() ve undo() yalnızca Main Thread'den çağrılır.
 *   CommandManager bu garantiyi sağlar.
 */
interface ICommand {
    fun execute()
    fun undo()
}
```

---

**Path:** `app/src/main/java/com/notia/engine/command/CommandManager.kt`

```kotlin
package com.notia.engine.command

import com.notia.domain.model.Page

/**
 * Undo/Redo yığınlarını yöneten merkezi state makinesi.
 *
 * KULLANIM KURALLARI:
 *   - Page state'i yalnızca execute() üzerinden değiştirilir.
 *   - currentPage'e dışarıdan doğrudan atama yapılmaz.
 *   - execute() çağrısı redo stack'ini temizler.
 *   - Stack boyutu maxStackSize ile sınırlıdır; en eski komutlar atılır.
 *
 * BELLEK:
 *   Page kopyası PersistentList structural sharing ile O(log n)
 *   maliyetlidir. Her komut tüm listeyi kopyalamaz [2].
 *
 * THREAD SAFETY:
 *   Bu sınıf thread-safe değildir.
 *   Yalnızca Main Thread'den çağrılmalıdır.
 *
 * @param initialPage   Başlangıç sayfa durumu.
 * @param maxStackSize  Undo/Redo stack'lerinin maksimum derinliği.
 */
class CommandManager(
    initialPage: Page,
    private val maxStackSize: Int = 100
) {
    /**
     * Güncel sayfa durumu.
     * Dışarıdan okunabilir, dışarıdan yazılamaz.
     * Yalnızca Command'lar updatePage() üzerinden değiştirebilir.
     */
    var currentPage: Page = initialPage
        private set

    private val undoStack = ArrayDeque<ICommand>()
    private val redoStack = ArrayDeque<ICommand>()

    /**
     * Komutu çalıştırır, Undo yığınına ekler, Redo yığınını temizler.
     * Yığın doluysa en eski komut FIFO ile düşürülür.
     */
    fun execute(command: ICommand) {
        command.execute()
        if (undoStack.size >= maxStackSize) {
            undoStack.removeFirst()
        }
        undoStack.addLast(command)
        redoStack.clear()
    }

    /**
     * Son komutu geri alır.
     * Undo yığını boşsa no-op.
     */
    fun undo() {
        val command = undoStack.removeLastOrNull() ?: return
        command.undo()
        redoStack.addLast(command)
    }

    /**
     * Son geri alınan komutu yeniden uygular.
     * Redo yığını boşsa no-op.
     */
    fun redo() {
        val command = redoStack.removeLastOrNull() ?: return
        command.execute()
        undoStack.addLast(command)
    }

    fun canUndo(): Boolean = undoStack.isNotEmpty()
    fun canRedo(): Boolean = redoStack.isNotEmpty()

    /**
     * Yeni sayfa açıldığında state ve yığınları temizler.
     */
    fun reset(newPage: Page) {
        currentPage = newPage
        undoStack.clear()
        redoStack.clear()
    }

    /**
     * Command'ların currentPage'i güncellemek için kullandığı
     * internal setter. Dışarıdan erişilemez.
     */
    internal fun updatePage(newPage: Page) {
        currentPage = newPage
    }
}
```

---

**Path:** `app/src/main/java/com/notia/engine/command/AddStrokeCommand.kt`

```kotlin
package com.notia.engine.command

import com.notia.domain.model.Stroke

/**
 * Tamamlanan bir Stroke'u belirtilen Layer'a ekler.
 *
 * ÇAĞRI NOKTASI:
 *   DynamicInputSampler ACTION_UP algıladığında [1]:
 *     1. endStroke() JNI çağrısı → C++ görsel mühürler [4]
 *     2. Kotlin elindeki StylusPoint listesinden Stroke objesi oluşturur
 *     3. AddStrokeCommand(layerId, stroke, manager) → execute()
 *
 * C++'TAN VERİ BEKLENMESİ YASAKTIR.
 *   Noktalar Kotlin'de zaten mevcuttur [1].
 *
 * LAYER HİYERARŞİSİ:
 *   Page doğrudan strokes tutmaz [2].
 *   Stroke'lar Page.layers[index].strokes içinde yaşar.
 *   layerId hangi Layer'a yazılacağını belirtir.
 *
 * UNDO:
 *   Fiziksel silme yapılmaz.
 *   isDeleted = true ile soft-delete uygulanır [2].
 *
 * @param layerId   Stroke'un ekleneceği Layer'ın ID'si.
 * @param stroke    Eklenecek Stroke objesi.
 * @param manager   CommandManager referansı.
 */
class AddStrokeCommand(
    private val layerId: String,
    private val stroke: Stroke,
    private val manager: CommandManager
) : ICommand {

    override fun execute() {
        val current = manager.currentPage
        val layerIndex = current.layers.indexOfFirst { it.id == layerId }
        if (layerIndex == -1) return

        val updatedLayer = current.layers[layerIndex].copy(
            strokes = current.layers[layerIndex].strokes.add(stroke)
        )
        manager.updatePage(
            current.copy(
                layers  = current.layers.set(layerIndex, updatedLayer),
                version = System.nanoTime()
            )
        )
    }

    override fun undo() {
        // Fiziksel silme değil — soft-delete [2]
        val current = manager.currentPage
        val layerIndex = current.layers.indexOfFirst { it.id == layerId }
        if (layerIndex == -1) return

        val updatedLayer = current.layers[layerIndex].copy(
            strokes = current.layers[layerIndex].strokes.mutate { list ->
                val iterator = list.listIterator()
                while (iterator.hasNext()) {
                    val s = iterator.next()
                    if (s.id == stroke.id) {
                        iterator.set(s.copy(isDeleted = true))
                    }
                }
            }
        )
        manager.updatePage(
            current.copy(
                layers  = current.layers.set(layerIndex, updatedLayer),
                version = System.nanoTime()
            )
        )
    }
}
```

---

**Path:** `app/src/main/java/com/notia/engine/command/DeleteStrokeCommand.kt`

```kotlin
package com.notia.engine.command

/**
 * Bir veya birden fazla Stroke'u soft-delete ile siler.
 *
 * KURAL: Fiziksel silme YAPILMAZ [2].
 *        isDeleted = true uygulanır.
 *        Undo durumunda isDeleted = false yapılarak geri getirilir.
 *
 * PERFORMANS:
 *   .map {} yerine PersistentList.mutate {} kullanılır.
 *   .map {} standart List<T> döndürür → type mismatch derleme hatası.
 *   .mutate {} PersistentList<T> döndürür → GC dostu, typesafe [2].
 *
 * CRDT UYUMU:
 *   Soft-delete, senkronizasyonda "bu silindi" bilgisinin
 *   kaybolmamasını garantiler [2].
 *
 * @param layerId     Stroke'ların bulunduğu Layer'ın ID'si.
 * @param strokeIds   Silinecek Stroke ID'leri.
 * @param manager     CommandManager referansı.
 */
class DeleteStrokeCommand(
    private val layerId: String,
    private val strokeIds: Set<String>,
    private val manager: CommandManager
) : ICommand {

    override fun execute() {
        applyDelete(deleted = true)
    }

    override fun undo() {
        applyDelete(deleted = false)
    }

    private fun applyDelete(deleted: Boolean) {
        val current = manager.currentPage
        val layerIndex = current.layers.indexOfFirst { it.id == layerId }
        if (layerIndex == -1) return

        val updatedLayer = current.layers[layerIndex].copy(
            strokes = current.layers[layerIndex].strokes.mutate { list ->
                val iterator = list.listIterator()
                while (iterator.hasNext()) {
                    val s = iterator.next()
                    if (s.id in strokeIds) {
                        iterator.set(s.copy(isDeleted = deleted))
                    }
                }
            }
        )
        manager.updatePage(
            current.copy(
                layers  = current.layers.set(layerIndex, updatedLayer),
                version = System.nanoTime()
            )
        )
    }
}
```

---

**Path:** `app/src/main/java/com/notia/engine/command/TransformStrokesCommand.kt`

```kotlin
package com.notia.engine.command

/**
 * Lasso ile seçilen Stroke'ları kalıcı olarak taşır.
 *
 * MİMARİ:
 *   Sürükleme SIRASINDA bu komut çalıştırılmaz.
 *   C++ tarafı SkMatrix::translate ile görsel illüzyon uygular.
 *   Parmak kalktığında (ACTION_UP) bu komut bir kez execute edilir.
 *
 * KOORDİNAT UZAYI:
 *   deltaX ve deltaY CANONICAL koordinat uzayındadır [1][2].
 *   Fiziksel ekran pikseli değildir.
 *   Zoom durumunda: canonicalDelta = physicalDelta / zoomScale
 *   Bu dönüşüm Faz 2'nin toCanonicalX/Y mantığıyla tutarlıdır [1].
 *
 * BELLEK STRATEJİSİ:
 *   StylusPoint listesine DOKUNULMAZ.
 *   Yalnızca transformOffsetX/Y güncellenir [2].
 *   C++ render anında SkMatrix::translate ile bu offset'i uygular.
 *
 * PERFORMANS:
 *   .map {} yerine .mutate {} — type mismatch ve GC baskısı önlenir [2].
 *
 * @param layerId     Stroke'ların bulunduğu Layer'ın ID'si.
 * @param strokeIds   Taşınacak Stroke ID'leri.
 * @param deltaX      Canonical X ekseni öteleme miktarı.
 * @param deltaY      Canonical Y ekseni öteleme miktarı.
 * @param manager     CommandManager referansı.
 */
class TransformStrokesCommand(
    private val layerId: String,
    private val strokeIds: Set<String>,
    private val deltaX: Float,
    private val deltaY: Float,
    private val manager: CommandManager
) : ICommand {

    override fun execute() {
        applyDelta(deltaX, deltaY)
    }

    override fun undo() {
        applyDelta(-deltaX, -deltaY)
    }

    private fun applyDelta(dx: Float, dy: Float) {
        val current = manager.currentPage
        val layerIndex = current.layers.indexOfFirst { it.id == layerId }
        if (layerIndex == -1) return

        val updatedLayer = current.layers[layerIndex].copy(
            strokes = current.layers[layerIndex].strokes.mutate { list ->
                val iterator = list.listIterator()
                while (iterator.hasNext()) {
                    val s = iterator.next()
                    if (s.id in strokeIds) {
                        iterator.set(
                            s.copy(
                                transformOffsetX = s.transformOffsetX + dx,
                                transformOffsetY = s.transformOffsetY + dy,
                                version          = System.nanoTime()
                            )
                        )
                    }
                }
            }
        )
        manager.updatePage(
            current.copy(
                layers  = current.layers.set(layerIndex, updatedLayer),
                version = System.nanoTime()
            )
        )
    }
}
```

---

## Dosya 2 — C++ Tool Renderer Arayüzü

**Path:** `app/src/main/cpp/tools/IToolRenderer.h`

```cpp
#pragma once

#include "include/core/SkRefCnt.h"
#include <vector>
#include <cstdint>

class SkCanvas;

// ─────────────────────────────────────────────────────────────────────────────
// VERİ YAPILARI
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Render katmanının kullandığı tek nokta yapısı.
 * JNI katmanındaki StylusPoint ile aynı layout'a sahiptir [1].
 */
struct RenderPoint {
    float   x;          // canonical koordinat (0–2000) [2]
    float   y;          // canonical koordinat (0–3000) [2]
    float   pressure;   // 0.0f – 1.0f, normalize edilmiş
    int64_t timestamp;  // nanoseconds [2]
};

/**
 * Bir stroke'un render parametreleri.
 * IToolRenderer::beginStroke() çağrısında iletilir.
 *
 * surfaceWidth / surfaceHeight:
 *   HighlighterRenderer::beginStroke içinde saveLayer için
 *   bounds hesabında kullanılır.
 *   DrawingEngine bu değerleri setSurfaceSize() üzerinden alır [4].
 */
struct StrokeParams {
    int   color;          // ARGB int
    float baseWidth;      // piksel cinsinden temel genişlik
    int   hardwareHz;     // 60 / 120 / 240
    int   surfaceWidth;   // fiziksel yüzey genişliği (px)
    int   surfaceHeight;  // fiziksel yüzey yüksekliği (px)
};

// ─────────────────────────────────────────────────────────────────────────────
// ABSTRACT TOOL RENDERER
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Tüm kalem renderer'larının uygulaması gereken soyut arayüz.
 *
 * BELLEK YÖNETİMİ:
 *   IToolRenderer, SkRefCnt'den türer.
 *   DrawingEngine ve ToolRendererFactory sk_sp<IToolRenderer> kullanır.
 *   std::unique_ptr KESİNLİKLE KULLANILAMAZ [4].
 *
 * KURAL:
 *   Yeni kalem tipi eklenirken bu sınıftan türetilir.
 *   Mevcut renderer sınıflarına DOKUNULMAZ.
 *
 * YAŞAM DÖNGÜSÜ:
 *   beginStroke(params)        → stroke başladı, Skia state hazırla
 *   addPoints(canvas, points)  → incremental render
 *   endStroke(canvas)          → final commit, buffer temizle
 *
 * THREAD SAFETY:
 *   Tüm fonksiyonlar Render Thread'den çağrılır [4].
 *   SkCanvas thread-safe değildir; çağrı noktası bunu garantiler.
 *
 * GELEN VERİ:
 *   addPoints'e gelen noktalar Faz 3'te Catmull-Rom'dan
 *   geçmiş haldedir [4]. Renderer tekrar yumuşatma yapmaz.
 */
class IToolRenderer : public SkRefCnt {
public:
    virtual ~IToolRenderer() = default;

    virtual void beginStroke(const StrokeParams& params) = 0;

    /**
     * Her nokta batch'inde çağrılır.
     * @param canvas  Null kontrolü caller sorumluluğundadır.
     * @param points  Catmull-Rom'dan geçmiş noktalar [4].
     */
    virtual void addPoints(SkCanvas*                       canvas,
                           const std::vector<RenderPoint>& points) = 0;

    /**
     * Stroke tamamlandığında çağrılır.
     * Kalan buffer flush edilir, dahili state temizlenir.
     */
    virtual void endStroke(SkCanvas* canvas) = 0;

    /**
     * Ghost noktaları geçici görsel katmana çizer.
     * Default implementasyon no-op.
     * Kalıcı buffer'a EKLENMEZ [4].
     */
    virtual void drawGhostPoints(SkCanvas*                       canvas,
                                 const std::vector<RenderPoint>& points)
    {
        (void)canvas;
        (void)points;
    }

protected:
    StrokeParams activeParams_{};

    /**
     * Pressure → piksel genişliği dönüşümü.
     * Formül: baseWidth * (0.3 + pressure * 0.7)
     * minFactor=0.3 → sıfır basınçta %30 genişlik
     * maxFactor=1.0 → tam basınçta tam genişlik
     */
    static float pressureToWidth(float pressure, float baseWidth) {
        constexpr float kMinFactor = 0.3f;
        constexpr float kMaxFactor = 1.0f;
        const float clamped = (pressure < 0.0f) ? 0.0f
                            : (pressure > 1.0f) ? 1.0f
                            : pressure;
        return baseWidth * (kMinFactor + clamped * (kMaxFactor - kMinFactor));
    }
};
```

---

## Dosya 3 — Ballpoint Renderer Header

**Path:** `app/src/main/cpp/tools/BallpointRenderer.h`

```cpp
#pragma once

#include "IToolRenderer.h"

// #include "include/core/SkCanvas.h"
// #include "include/core/SkPaint.h"
// #include "include/core/SkVertices.h"

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
 *   Her Catmull-Rom noktası [4] için teğet vektörü hesaplanır.
 *   Teğete dik normal ile sol/sağ vertex çifti üretilir.
 *   Segment birleşimlerinde gerçek Miter Join uygulanır:
 *     N = normalize(T1 + T2)
 *     P ± N × (w / (2 × cos(θ/2)))
 *   Açı limiti (kMiterLimit) aşılırsa Bevel Join'e geçilir.
 *   Tüm vertex'ler SkVertices (kTriangleStrip_VertexMode) ile
 *   canvas->drawVertices() üzerinden GPU'ya gönderilir.
 *
 * NEDEN MESH?
 *   drawLine veya sabit SkPath ile variable-width çizim eklem
 *   noktalarında kırılma üretir. Mesh bu sorunu köklü çözer ve
 *   ileride eklenecek FountainPenRenderer için de temel oluşturur.
 *
 * BELLEK:
 *   Tüm noktaları biriktiren strokeBuffer_ KULLANILMAZ.
 *   Miter Join için yalnızca son teğet ve vertex çifti saklanır.
 *   meshVertices_ her flushMesh() çağrısında temizlenir.
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
```

---

## Dosya 4 — Ballpoint Renderer Implementation

**Path:** `app/src/main/cpp/tools/BallpointRenderer.cpp`

```cpp
#include "BallpointRenderer.h"
#include <android/log.h>
#include <cmath>

// #include "include/core/SkCanvas.h"
// #include "include/core/SkPaint.h"
// #include "include/core/SkVertices.h"

#define LOG_TAG "BallpointRenderer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
    /**
     * Miter Join limit çarpanı.
     * scale > kMiterLimit * halfWidth ise Bevel Join uygulanır.
     * Aşılırsa vertex teorik olarak sonsuz uzağa fırlayabilir.
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

    // Skia SkPaint hazırlığı (header eklenince aktif edilecek):
    // strokePaint_.reset();
    // strokePaint_.setAntiAlias(true);
    // strokePaint_.setStyle(SkPaint::kFill_Style);  // mesh fill
    // strokePaint_.setBlendMode(SkBlendMode::kSrcOver);
    // strokePaint_.setColor(params.color);
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
    // meshVertices_'e EKLENMEZ; kalıcı stroke'u kirletmez [4].
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    // std::vector<float> ghostMesh;
    // ... appendSegment mantığıyla ghost mesh üret ...
    // SkPaint ghostPaint = strokePaint_;
    // ghostPaint.setAlphaf(kGhostOpacity);
    // canvas->drawVertices(SkVertices::MakeCopy(...), SkBlendMode::kSrcOver, ghostPaint);

    (void)canvas;
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
        // scale = halfWidth / cos(θ/2) = halfWidth / dot(N_current, miterNormal)
        //
        // T_prev = (prevTx_, prevTy_), T_current = (tx, ty)
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

    // Skia implementasyonu (header eklenince aktif edilecek):
    //
    // const int vertexCount = static_cast<int>(meshVertices_.size() / 2);
    // std::vector<SkPoint> skPoints(vertexCount);
    // for (int i = 0; i < vertexCount; ++i) {
    //     skPoints[i] = SkPoint::Make(meshVertices_[i * 2],
    //                                 meshVertices_[i * 2 + 1]);
    // }
    // sk_sp<SkVertices> verts = SkVertices::MakeCopy(
    //     SkVertices::kTriangleStrip_VertexMode,
    //     vertexCount,
    //     skPoints.data(),
    //     nullptr,
    //     nullptr
    // );
    // if (verts) {
    //     canvas->drawVertices(verts, SkBlendMode::kSrcOver, strokePaint_);
    // }

    (void)canvas;
    LOGD("BallpointRenderer::flushMesh: %zu vertices.", meshVertices_.size());

    meshVertices_.clear();
}
```

---

## Dosya 5 — Highlighter Renderer Header

**Path:** `app/src/main/cpp/tools/HighlighterRenderer.h`

```cpp
#pragma once

#include "IToolRenderer.h"
#include <vector>

// #include "include/core/SkCanvas.h"
// #include "include/core/SkPaint.h"
// #include "include/core/SkPath.h"

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
 *
 *   Sorun:
 *     Faz 3 render loop ekranı temizlemiyor (incremental) [4].
 *     Şeffaf snapshot her frame ana canvas'a üst üste basılırsa
 *     alpha birikir ve çizgi katılaşır (stacking bomb):
 *       Frame 1: %50 opacity
 *       Frame 2: 0.5 + 0.5*(1-0.5) = %75
 *       Frame N: ~%100
 *
 *   Çözüm — canvas->saveLayer():
 *     Skia tam bu senaryo için saveLayer mekanizmasını sağlar.
 *     Off-screen surface elle yönetmeye gerek kalmaz.
 *
 *     beginStroke → canvas->saveLayer(bounds, nullptr)
 *       Skia dahili geçici bir katman açar.
 *       Tüm çizimler bu katmana gider, ana canvas'a dokunulmaz.
 *
 *     addPoints → saveLayer canvas'ına SrcOver ile çiz.
 *       Kullanıcı anlık görür (sıfır gecikme) [4].
 *       Aynı vuruş içinde üst üste kararma yok (SrcOver, tek katman).
 *
 *     endStroke → canvas->restore()
 *       Skia geçici katmanı kMultiply blend ile ana canvas'a
 *       tek seferde mühürler. Stacking bomb imkânsız.
 *
 *   Neden Off-Screen Surface değil SaveLayer?
 *     Off-screen surface ile her addPoints'te snapshot alıp ana
 *     canvas'a basınca — canvas temizlenmediğinden — önceki
 *     frame'in üzerine yenisi biniyor, alpha birikimi oluşuyor.
 *     saveLayer'da ana canvas'a dokunulmaz; restore tek seferlik.
 *
 * KURAL: BallpointRenderer'a DOKUNULMAZ.
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
     * Skia header'ı eklenince SkRect olarak tanımlanacak.
     */
    // SkRect layerBounds_;

    /**
     * saveLayer canvas'ına çizim için paint.
     * Style: kStroke, Cap: kSquare, BlendMode: kSrcOver.
     * beginStroke'ta hazırlanır.
     */
    // SkPaint offscreenPaint_;

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
```

---

## Dosya 6 — Highlighter Renderer Implementation

**Path:** `app/src/main/cpp/tools/HighlighterRenderer.cpp`

```cpp
#include "HighlighterRenderer.h"
#include <android/log.h>

// #include "include/core/SkCanvas.h"
// #include "include/core/SkPaint.h"
// #include "include/core/SkPath.h"

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

    // saveLayer mimarisi:
    //   Tüm vuruş boyunca tek bir geçici Skia katmanı açılır.
    //   addPoints bu katmana yazar, endStroke'ta kMultiply ile mühürlenir.
    //   Stacking bomb imkânsız — ana canvas'a addPoints hiç dokunmaz.
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    //
    // layerBounds_ = SkRect::MakeWH(
    //     static_cast<float>(params.surfaceWidth),
    //     static_cast<float>(params.surfaceHeight)
    // );
    //
    // SkPaint layerPaint;
    // layerPaint.setBlendMode(SkBlendMode::kMultiply);
    // canvas->saveLayer(&layerBounds_, &layerPaint);
    // layerSaved_ = true;
    //
    // // Off-screen çizim paint'i
    // offscreenPaint_.reset();
    // offscreenPaint_.setAntiAlias(true);
    // offscreenPaint_.setStyle(SkPaint::kStroke_Style);
    // offscreenPaint_.setBlendMode(SkBlendMode::kSrcOver);
    // offscreenPaint_.setStrokeWidth(params.baseWidth * kWidthMultiplier);
    // offscreenPaint_.setStrokeCap(SkPaint::kSquare_Cap);
    // offscreenPaint_.setAlphaf(kHighlighterOpacity);
    // offscreenPaint_.setColor(params.color);
    //
    // NOT: beginStroke canvas parametresi almıyor.
    // DrawingEngine bu çağrıyı canvas hazır olduğunda yapmalı
    // veya saveLayer ilk addPoints'te lazy açılabilir.
    // (Seçim: DrawingEngine entegrasyonunda netleştirilecek)
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

    // Lazy saveLayer: beginStroke canvas almadığı için
    // ilk addPoints'te açılıyor.
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    // if (!layerSaved_) {
    //     SkPaint layerPaint;
    //     layerPaint.setBlendMode(SkBlendMode::kMultiply);
    //     canvas->saveLayer(&layerBounds_, &layerPaint);
    //     layerSaved_ = true;
    // }

    // saveLayer canvas'ına SrcOver ile çiz.
    // Ana canvas'a dokunulmaz — stacking bomb yok.
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

    // canvas->restore() ile saveLayer kapatılır.
    // Skia geçici katmanı kMultiply blend ile ana canvas'a
    // tek seferde mühürler. Üst üste koyulaşma bu noktada
    // doğal olarak oluşur (fosforlu kalem beklenen davranışı).
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    // if (layerSaved_) {
    //     canvas->restore();
    //     layerSaved_ = false;
    // }

    (void)canvas;
    layerSaved_ = false;

    LOGD("HighlighterRenderer::endStroke: layer composited with kMultiply.");
}

// ─────────────────────────────────────────────────────────────────────────────

void HighlighterRenderer::drawGhostPoints(SkCanvas*                       canvas,
                                          const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr || points.empty()) {
        return;
    }

    // Ghost noktalar mevcut saveLayer katmanına YAZILMAZ.
    // Ayrı geçici bir katman açılır, ghost çizilir, hemen kapatılır.
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    // SkPaint ghostLayerPaint;
    // ghostLayerPaint.setBlendMode(SkBlendMode::kSrcOver);
    // canvas->saveLayer(&layerBounds_, &ghostLayerPaint);
    // drawToLayer(canvas, points, kGhostOpacity);
    // canvas->restore();

    (void)canvas;
    LOGD("HighlighterRenderer::drawGhostPoints: %zu ghost points.", points.size());
}

// ─────────────────────────────────────────────────────────────────────────────

void HighlighterRenderer::drawToLayer(SkCanvas*                       canvas,
                                      const std::vector<RenderPoint>& points,
                                      float                           opacity)
{
    if (points.empty()) return;

    // Tüm noktaları tek SkPath — tek draw call.
    // Skia implementasyonu (header eklenince aktif edilecek):
    //
    // SkPath path;
    // path.moveTo(points[0].x, points[0].y);
    // for (size_t i = 1; i < points.size(); ++i) {
    //     path.lineTo(points[i].x, points[i].y);
    // }
    // SkPaint paint = offscreenPaint_;
    // paint.setAlphaf(opacity);
    // canvas->drawPath(path, paint);

    (void)canvas;
    (void)opacity;
    LOGD("HighlighterRenderer::drawToLayer: %zu points.", points.size());
}
```

---

## Dosya 7 — Renderer Seçim Fabrikası

**Path:** `app/src/main/cpp/tools/ToolRendererFactory.h`

```cpp
#pragma once

#include "IToolRenderer.h"
#include "BallpointRenderer.h"
#include "HighlighterRenderer.h"
#include "include/core/SkRefCnt.h"

/**
 * ToolType ordinal değerine göre doğru renderer'ı oluşturur.
 *
 * Kotlin ToolType enum sırası [2]:
 *   0 = BALLPOINT_PEN
 *   1 = FOUNTAIN_PEN
 *   2 = HIGHLIGHTER
 *   3 = MARKER
 *   4 = ERASER
 *
 * BELLEK YÖNETİMİ:
 *   Dönüş tipi sk_sp<IToolRenderer>.
 *   std::unique_ptr KESİNLİKLE KULLANILAMAZ [4].
 *   IToolRenderer SkRefCnt'den türediği için referans sayacı
 *   Skia tarafından yönetilir.
 *
 * KURAL:
 *   Yeni tool = yalnızca bu switch'e yeni case.
 *   Mevcut case'lere DOKUNULMAZ.
 *   ToolType ordinal sırası değişirse switch güncellenir.
 */
class ToolRendererFactory {
public:
    /**
     * @param toolTypeOrd  Kotlin ToolType.ordinal değeri.
     * @return             sk_sp<IToolRenderer>.
     *                     Tanımsız ordinal → BallpointRenderer (safe default).
     */
    static sk_sp<IToolRenderer> create(int toolTypeOrd) {
        switch (toolTypeOrd) {
            case 0:  // BALLPOINT_PEN
                return sk_make_sp<BallpointRenderer>();

            case 1:  // FOUNTAIN_PEN
                // FountainPenRenderer henüz implemente edilmedi.
                // Mesh altyapısı BallpointRenderer ile paylaşılacak;
                // hız-duyarlı kalınlık ve mürekkep efekti eklenecek.
                return sk_make_sp<BallpointRenderer>();

            case 2:  // HIGHLIGHTER
                return sk_make_sp<HighlighterRenderer>();

            case 3:  // MARKER
                // MarkerRenderer henüz implemente edilmedi.
                // Opak, geniş, sabit genişlik — SrcOver blend.
                return sk_make_sp<BallpointRenderer>();

            case 4:  // ERASER
                // EraserRenderer henüz implemente edilmedi.
                // Mimari: C++ → kDstOut blend (görsel),
                //         Kotlin → DeleteStrokeCommand soft-delete (model) [2].
                return sk_make_sp<BallpointRenderer>();

            default:
                return sk_make_sp<BallpointRenderer>();
        }
    }

    ToolRendererFactory()  = delete;
    ~ToolRendererFactory() = delete;
};
```

---

## Faz 3 Güncelleme Notları

**Path:** `app/src/main/cpp/engine/DrawingEngine.h`

```cpp
// ESKİ (Anayasa ihlali) [4]:
std::unique_ptr<IToolRenderer> activeRenderer_;

// DOĞRU:
sk_sp<IToolRenderer> activeRenderer_;
```

**Path:** `app/src/main/cpp/engine/DrawingEngine.cpp` — `nativeBeginStroke` içi:

```cpp
// Tool değiştirme güvenliği:
// Çizim ortasında tool değişirse önceki stroke kapanmadan
// yeni renderer oluşturulmasın.
if (activeStroke_.isActive && activeRenderer_) {
    activeRenderer_->endStroke(skCanvas_);
    activeStroke_.isActive = false;
}
activeRenderer_ = ToolRendererFactory::create(toolTypeOrd);
```

---

## Kural Özeti

| Kural | Zorunluluk |
|---|---|
| Tüm state değişikliği `CommandManager.execute()` üzerinden | **ZORUNLU** |
| `currentPage` dışarıdan mutate edilemez | **ZORUNLU** |
| Command'lar `layerId` parametresiyle çalışır; `Page.layers[index].strokes`'a yazar | **ZORUNLU** |
| JNI köprüsü tek yönlü: Kotlin → C++. Noktalar geri alınmaz | **ZORUNLU** |
| `AddStrokeCommand` ACTION_UP anında Kotlin'deki ham noktalarla çalışır | **ZORUNLU** |
| `PersistentList` mutasyonunda `.map {}` değil `.mutate {}` kullanılır | **ZORUNLU** |
| `IToolRenderer` ve tüm türevleri `SkRefCnt`'den türer | **ZORUNLU** |
| `ToolRendererFactory` ve `DrawingEngine` `sk_sp<IToolRenderer>` kullanır | **ZORUNLU** |
| `std::unique_ptr` Skia nesneleri için kullanılamaz | **YASAK** |
| Yeni tool = yeni strateji sınıfı; mevcut renderer'a dokunulmaz | **ZORUNLU** |
| `HighlighterRenderer` `saveLayer/restore` mimarisi kullanır | **ZORUNLU** |
| `HighlighterRenderer` `kMultiply` yalnızca `restore()` anında uygulanır | **ZORUNLU** |
| `BallpointRenderer` triangle strip mesh kullanır; `drawLine` veya sabit `SkPath` kullanılamaz | **ZORUNLU** |
| Miter Join: `N = normalize(T1+T2)`, limit aşılınca Bevel | **ZORUNLU** |
| Ghost noktalar hiçbir kalıcı buffer'a eklenmez | **ZORUNLU** |
| `DeleteStrokeCommand` → `isDeleted=true` soft-delete | **ZORUNLU** |
| `TransformStrokesCommand` canonical koordinat uzayında çalışır | **ZORUNLU** |
| Tool değiştirme anında aktif stroke varsa önce `endStroke` çağrılır | **ZORUNLU** |
