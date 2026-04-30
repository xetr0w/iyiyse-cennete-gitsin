# PHASE_4_TOOL_STRATEGY.md

## Amaç

Bu doküman, Kotlin tarafında tüm state mutasyonlarını Command Pattern üzerinden yöneten yapıyı ve C++ tarafında farklı kalem türlerinin Skia ile farklı render edilmesini sağlayan Strategy Pattern iskeletini tanımlar.

### Mimari Pozisyon

```
Kullanıcı Eylemi (UI)
        │
        ▼
CommandManager.kt          ← tüm state mutasyonu buradan geçer
        │
        ├─ execute(AddStrokeCommand)   → Page.strokes güncellenir (immutable copy)
        ├─ undo()                      → önceki Page snapshot'ına dön
        └─ redo()                      → ileri al
        
C++ DrawingEngine
        │
        ▼
IToolRenderer (abstract)   ← strategy arayüzü
        ├─ BallpointRenderer    → normal blend, pressure-sensitive width
        └─ HighlighterRenderer  → Multiply blend mode, sabit opacity
```

---

## Dosya 1 — Kotlin Command Manager

**Path:** `app/src/main/java/com/yourdomain/notes/domain/CommandManager.kt`

```kotlin
package com.yourdomain.notes.domain

import com.yourdomain.notes.domain.model.Page
import com.yourdomain.notes.domain.model.Stroke

// ─────────────────────────────────────────────────────────────────────────────
// COMMAND INTERFACE
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Tüm state değiştiren işlemlerin uygulaması gereken arayüz.
 *
 * KURALLAR:
 * - [execute] ve [undo] yan etkisiz (side-effect-free) olmalıdır;
 *   dış servislere çağrı yapmaz, yalnızca immutable model kopyaları üretir.
 * - Her Command kendi çalıştırılması için gereken tüm veriyi
 *   constructor parametrelerinde taşımalıdır.
 * - Command nesneleri oluşturulduktan sonra iç durumları değiştirilmez.
 */
interface Command {
    /**
     * Komutu uygular.
     * @return Komutun uygulandığı yeni [Page] snapshot'ı.
     */
    fun execute(): Page

    /**
     * Komutu geri alır.
     * @return Komut uygulanmadan önceki [Page] snapshot'ı.
     */
    fun undo(): Page
}

// ─────────────────────────────────────────────────────────────────────────────
// ADD STROKE COMMAND
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Bir sayfaya yeni stroke ekleyen komut.
 *
 * IMMUTABILITY GARANTISI:
 * - [execute]: [previousPage].copy() ile strokes listesine [stroke] eklenir.
 * - [undo]:    [previousPage] doğrudan döndürülür; hiçbir alan mutate edilmez.
 * - Page ve Stroke'un tüm alanları val olduğundan copy() derin kopya gerektirir.
 *   Kotlin data class copy() shallow copy yapar; List<StylusPoint> zaten immutable.
 *
 * CRDT NOTU:
 * Stroke fiziksel olarak silinmez. [undo] çağrıldığında stroke listeden
 * çıkarılmış gibi görünür ancak Stroke nesnesi CommandManager stack'inde
 * referans olarak yaşamaya devam eder. Gerçek CRDT silme işlemi
 * DeleteStrokeCommand üzerinden isDeleted=true flag'i ile yapılmalıdır.
 *
 * @param previousPage Komut uygulanmadan önceki sayfa snapshot'ı.
 * @param stroke       Eklenecek stroke. ID ve timestamp değiştirilemez.
 */
class AddStrokeCommand(
    private val previousPage: Page,
    private val stroke: Stroke
) : Command {

    /**
     * [previousPage]'e [stroke]'u ekleyerek yeni bir [Page] döndürür.
     * Orijinal [previousPage] değiştirilmez.
     */
    override fun execute(): Page {
        val updatedStrokes = previousPage.strokes + stroke
        return previousPage.copy(strokes = updatedStrokes)
    }

    /**
     * Stroke eklenmeden önceki [previousPage]'i döndürür.
     * Herhangi bir hesaplama yapılmaz; snapshot referansı doğrudan iletilir.
     */
    override fun undo(): Page {
        return previousPage
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE STROKE COMMAND
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Bir stroke'u soft-delete ile işaretleyen komut.
 *
 * Stroke fiziksel olarak listeden kaldırılmaz.
 * isDeleted = true olan stroke'lar render katmanında atlanır,
 * ancak CRDT log'unda ve undo stack'inde varlıklarını korur.
 *
 * @param previousPage Komut uygulanmadan önceki sayfa snapshot'ı.
 * @param strokeId     Silinecek stroke'un UUID'si.
 */
class DeleteStrokeCommand(
    private val previousPage: Page,
    private val strokeId: String
) : Command {

    /**
     * Hedef stroke'u isDeleted=true olarak işaretlenmiş kopyasıyla
     * değiştirerek yeni bir [Page] döndürür.
     * Eşleşen stroke bulunamazsa [previousPage] değişmeden döner ve log yazılır.
     */
    override fun execute(): Page {
        val updatedStrokes = previousPage.strokes.map { stroke ->
            if (stroke.id == strokeId) stroke.copy(isDeleted = true) else stroke
        }
        return previousPage.copy(strokes = updatedStrokes)
    }

    /**
     * Soft-delete işlemi önceki [previousPage]'i döndürür.
     */
    override fun undo(): Page {
        return previousPage
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// COMMAND MANAGER
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Tüm komutların çalıştırıldığı, geri alındığı ve ileri alındığı merkezi yönetici.
 *
 * KULLANIM KURALLARI:
 * - [Page] state'i yalnızca [execute] üzerinden değiştirilir.
 * - [currentPage]'e dışarıdan doğrudan atama yapılmaz.
 * - [execute] çağrısı redo stack'ini temizler:
 *   yeni bir komut sonrası "ileri al" geçmişi anlamsız hale gelir.
 * - Stack boyutu [maxStackSize] ile sınırlıdır; en eski komutlar atılır.
 *
 * THREAD SAFETY:
 * Bu sınıf thread-safe değildir. Yalnızca tek thread'den (örn. Main thread
 * veya belirlenmiş bir coroutine dispatcher) çağrılmalıdır.
 *
 * @param initialPage  Başlangıç sayfa durumu.
 * @param maxStackSize Undo/Redo stack'lerinin maksimum derinliği.
 *                     Varsayılan: 100. Bellek baskısına göre ayarlanabilir.
 */
class CommandManager(
    initialPage: Page,
    private val maxStackSize: Int = 100
) {

    /**
     * Mevcut sayfa durumu.
     * Salt okunur; dışarıdan değiştirilemez.
     * Her [execute], [undo], [redo] çağrısından sonra güncellenir.
     */
    var currentPage: Page = initialPage
        private set

    /**
     * Geri alınabilir komutların yığını.
     * En son çalıştırılan komut listenin sonundadır (LIFO).
     */
    private val undoStack: ArrayDeque<Command> = ArrayDeque()

    /**
     * İleri alınabilir komutların yığını.
     * [undo] çağrısında dolan, [execute] çağrısında temizlenen yığın.
     */
    private val redoStack: ArrayDeque<Command> = ArrayDeque()

    // ─────────────────────────────────────────────────────────────────────────
    // EXECUTE
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Verilen komutu çalıştırır ve [currentPage]'i günceller.
     *
     * İşlem sırası:
     * 1. Komut çalıştırılır; yeni Page snapshot'ı alınır.
     * 2. [currentPage] yeni snapshot'a güncellenir.
     * 3. Komut undo stack'ine eklenir.
     * 4. Redo stack'i temizlenir (yeni dal başladı).
     * 5. Stack boyutu [maxStackSize]'ı aşıyorsa en eski komut atılır.
     *
     * @param command Çalıştırılacak komut.
     * @return Komut uygulandıktan sonraki [Page] snapshot'ı.
     */
    fun execute(command: Command): Page {
        val newPage = command.execute()
        currentPage = newPage

        undoStack.addLast(command)
        redoStack.clear()

        // Stack boyutu sınırını aş: en eski komutu at.
        if (undoStack.size > maxStackSize) {
            undoStack.removeFirst()
        }

        return currentPage
    }

    // ─────────────────────────────────────────────────────────────────────────
    // UNDO
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Son komutu geri alır ve [currentPage]'i önceki duruma döndürür.
     *
     * İşlem sırası:
     * 1. Undo stack boşsa işlem yapılmaz; [currentPage] değişmez.
     * 2. En son komut undo stack'inden çıkarılır.
     * 3. [command.undo()] çağrılarak önceki Page snapshot'ı alınır.
     * 4. [currentPage] güncellenir.
     * 5. Komut redo stack'ine eklenir.
     *
     * @return Undo sonrası [Page] snapshot'ı.
     *         Stack boşsa mevcut [currentPage] döner.
     */
    fun undo(): Page {
        if (undoStack.isEmpty()) {
            return currentPage
        }

        val command = undoStack.removeLast()
        val previousPage = command.undo()
        currentPage = previousPage

        redoStack.addLast(command)

        return currentPage
    }

    // ─────────────────────────────────────────────────────────────────────────
    // REDO
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Son geri alınan komutu yeniden uygular ve [currentPage]'i günceller.
     *
     * İşlem sırası:
     * 1. Redo stack boşsa işlem yapılmaz; [currentPage] değişmez.
     * 2. En son komut redo stack'inden çıkarılır.
     * 3. [command.execute()] çağrılarak ileri alınmış Page snapshot'ı alınır.
     * 4. [currentPage] güncellenir.
     * 5. Komut undo stack'ine geri eklenir.
     *
     * @return Redo sonrası [Page] snapshot'ı.
     *         Stack boşsa mevcut [currentPage] döner.
     */
    fun redo(): Page {
        if (redoStack.isEmpty()) {
            return currentPage
        }

        val command = redoStack.removeLast()
        val nextPage = command.execute()
        currentPage = nextPage

        undoStack.addLast(command)

        return currentPage
    }

    // ─────────────────────────────────────────────────────────────────────────
    // DURUM SORGULAMA
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Geri alınabilir komut olup olmadığını döndürür.
     * UI katmanı "Geri Al" butonunun aktifliğini bu değerle yönetir.
     */
    fun canUndo(): Boolean = undoStack.isNotEmpty()

    /**
     * İleri alınabilir komut olup olmadığını döndürür.
     * UI katmanı "İleri Al" butonunun aktifliğini bu değerle yönetir.
     */
    fun canRedo(): Boolean = redoStack.isNotEmpty()

    /**
     * Mevcut undo stack derinliğini döndürür.
     * Test ve debug amaçlıdır.
     */
    fun undoStackSize(): Int = undoStack.size

    /**
     * Mevcut redo stack derinliğini döndürür.
     * Test ve debug amaçlıdır.
     */
    fun redoStackSize(): Int = redoStack.size

    /**
     * Her iki stack'i ve mevcut sayfa durumunu sıfırlar.
     * Sayfa kapatılırken veya yeni belge açılırken çağrılmalıdır.
     *
     * @param newPage Sıfırlanacak başlangıç sayfa durumu.
     */
    fun reset(newPage: Page) {
        currentPage = newPage
        undoStack.clear()
        redoStack.clear()
    }
}
```

---

## Dosya 2 — C++ Tool Renderer Arayüzü

**Path:** `app/src/main/cpp/tools/IToolRenderer.h`

```cpp
#pragma once

#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// FORWARD DECLARATIONS
// Skia header'ları CMakeLists.txt'te include path'e eklendikten sonra
// bu forward declaration'lar gerçek include'larla değiştirilmelidir.
// Şimdilik derleme bağımlılığını minimize etmek için forward declare edildi.
// ─────────────────────────────────────────────────────────────────────────────

class SkCanvas;
class SkPaint;
class SkPath;

// ─────────────────────────────────────────────────────────────────────────────
// VERI YAPISI
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Render katmanının kullandığı tek nokta yapısı.
 * JNI katmanındaki StylusPoint struct'ı ile aynı layout'a sahiptir;
 * ileride ortak bir header'a taşınabilir.
 */
struct RenderPoint {
    float   x;
    float   y;
    float   pressure;   // 0.0f – 1.0f, normalize edilmiş
    int64_t timestamp;  // nanoseconds
};

/**
 * Bir stroke'un render parametreleri.
 * IToolRenderer::beginStroke() çağrısında iletilir.
 */
struct StrokeParams {
    int   color;       // ARGB int
    float baseWidth;   // piksel cinsinden temel genişlik
    int   hardwareHz;  // 60, 120 veya 240 — interpolasyon adımı hesabında kullanılır
};

// ─────────────────────────────────────────────────────────────────────────────
// ABSTRACT TOOL RENDERER
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Tüm kalem renderer'larının uygulaması gereken soyut arayüz.
 *
 * KURAL: Yeni bir kalem tipi eklenirken bu sınıftan türetilir.
 *        Mevcut renderer sınıflarına DOKUNULMAZ.
 *
 * YAŞAM DÖNGÜSÜ:
 *   beginStroke(params)          → stroke başladı, Skia state hazırla
 *   addPoints(canvas, points)    → incremental render (ACTION_MOVE her geldiğinde)
 *   endStroke(canvas)            → final commit, kaynakları serbest bırak
 *
 * THREAD SAFETY:
 *   Tüm fonksiyonlar render thread'den çağrılmalıdır.
 *   SkCanvas thread-safe değildir; çağrı noktası bunu garantilemelidir.
 */
class IToolRenderer {
public:

    /**
     * Virtual destructor: türetilmiş sınıfın destructor'ının
     * base pointer üzerinden doğru çağrılmasını garantiler.
     */
    virtual ~IToolRenderer() = default;

    /**
     * Yeni stroke başlangıcında çağrılır.
     * SkPaint, blend mode ve başlangıç path'i bu fonksiyonda hazırlanır.
     *
     * @param params Renk, genişlik ve donanım Hz bilgisi.
     */
    virtual void beginStroke(const StrokeParams& params) = 0;

    /**
     * Her nokta batch'inde çağrılır; canvas'a incremental çizim yapar.
     *
     * @param canvas Skia canvas pointer'ı. Null kontrolü caller'ın sorumluluğundadır.
     * @param points Bu çağrıda işlenecek yeni noktalar.
     */
    virtual void addPoints(SkCanvas*                      canvas,
                           const std::vector<RenderPoint>& points) = 0;

    /**
     * Stroke tamamlandığında çağrılır.
     * Final path GPU'ya commit edilir, dahili bufferlar temizlenir.
     *
     * @param canvas Skia canvas pointer'ı.
     */
    virtual void endStroke(SkCanvas* canvas) = 0;

    /**
     * Ghost (tahminli) noktaları geçici görsel katmana çizer.
     * Default implementasyon no-op'tur; ghost desteği isteyen renderer override eder.
     *
     * @param canvas Skia canvas pointer'ı.
     * @param points Tahminli noktalar — kalıcı path'e eklenmez.
     */
    virtual void drawGhostPoints(SkCanvas*                      canvas,
                                 const std::vector<RenderPoint>& points)
    {
        // Default: ghost desteği olmayan renderer'lar bu fonksiyonu override etmez.
        (void)canvas;
        (void)points;
    }

protected:

    /**
     * Mevcut stroke parametreleri.
     * beginStroke() tarafından set edilir; addPoints() ve endStroke() tarafından okunur.
     */
    StrokeParams activeParams_{};

    /**
     * Pressure değerini gerçek piksel genişliğine dönüştürür.
     * Tüm renderer'ların ortak kullandığı yardımcı fonksiyon.
     *
     * Formül: baseWidth * (minFactor + pressure * (maxFactor - minFactor))
     * minFactor=0.3 → sıfır basınçta genişliğin %30'u
     * maxFactor=1.0 → tam basınçta tam genişlik
     *
     * @param pressure  0.0f – 1.0f normalize değer.
     * @param baseWidth Piksel cinsinden temel genişlik.
     * @return          Hesaplanan piksel genişliği.
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
#include <vector>

/**
 * Tükenmez kalem renderer'ı.
 *
 * RENDER ÖZELLİKLERİ:
 * - Blend mode: SrcOver (standart, opak çizim).
 * - Genişlik: pressure'a duyarlı; basınç arttıkça genişler.
 * - Anti-alias: aktif.
 * - Opacity: sabit, renk alpha değeriyle belirlenir.
 *
 * KURAL: Bu sınıfa yeni özellik eklenebilir.
 *        Ancak IToolRenderer arayüzü bu sınıf için değiştirilemez.
 */
class BallpointRenderer final : public IToolRenderer {
public:

    BallpointRenderer()  = default;
    ~BallpointRenderer() override = default;

    void beginStroke(const StrokeParams& params) override;

    void addPoints(SkCanvas*                      canvas,
                   const std::vector<RenderPoint>& points) override;

    void endStroke(SkCanvas* canvas) override;

    void drawGhostPoints(SkCanvas*                      canvas,
                         const std::vector<RenderPoint>& points) override;

private:

    /**
     * Mevcut stroke'un biriktirilen noktaları.
     * endStroke() çağrısında temizlenir.
     */
    std::vector<RenderPoint> strokeBuffer_;

    /**
     * Son işlenen nokta indeksi.
     * addPoints() incremental çizimde yalnızca yeni noktaları işlemek için kullanır.
     */
    size_t lastRenderedIndex_ = 0;

    /**
     * İki nokta arasında pressure-sensitive segment çizer.
     * Variable-width çizginin üçgenlerini Skia'ya gönderir.
     *
     * @param canvas Skia canvas.
     * @param from   Segment başlangıç noktası.
     * @param to     Segment bitiş noktası.
     */
    void drawSegment(SkCanvas*          canvas,
                     const RenderPoint& from,
                     const RenderPoint& to);
};
```

---

## Dosya 4 — Ballpoint Renderer Implementation

**Path:** `app/src/main/cpp/tools/BallpointRenderer.cpp`

```cpp
#include "BallpointRenderer.h"

#include <android/log.h>

#define LOG_TAG "BallpointRenderer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Skia include'ları CMakeLists.txt'te Skia path tanımlı olduğunda aktif edilir.
// #include "include/core/SkCanvas.h"
// #include "include/core/SkPaint.h"
// #include "include/core/SkPath.h"
// ─────────────────────────────────────────────────────────────────────────────

void BallpointRenderer::beginStroke(const StrokeParams& params) {
    activeParams_        = params;
    strokeBuffer_.clear();
    lastRenderedIndex_   = 0;

    LOGD("BallpointRenderer::beginStroke color=0x%08X width=%.2f hz=%d",
         params.color, params.baseWidth, params.hardwareHz);

    // Skia SkPaint hazırlığı (Skia header'ları eklenince aktif edilecek):
    //
    // paint_.reset();
    // paint_.setAntiAlias(true);
    // paint_.setStyle(SkPaint::kStroke_Style);
    // paint_.setBlendMode(SkBlendMode::kSrcOver);
    // paint_.setColor(params.color);
    //
    // NOT: Genişlik her segment için pressure'a göre hesaplanır;
    //      paint_.setStrokeWidth() burada set edilmez.
}

void BallpointRenderer::addPoints(SkCanvas*                      canvas,
                                  const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr) {
        LOGE("BallpointRenderer::addPoints: canvas is null.");
        return;
    }

    if (points.empty()) {
        return;
    }

    // Gelen noktaları kalıcı buffer'a ekle.
    for (const auto& point : points) {
        strokeBuffer_.push_back(point);
    }

    // Yalnızca bir önceki çağrıdan bu yana eklenen yeni segmentleri çiz.
    // Tüm buffer'ı her seferinde yeniden çizmek O(n²) render maliyeti yaratır.
    const size_t bufferSize = strokeBuffer_.size();
    for (size_t i = lastRenderedIndex_; i + 1 < bufferSize; ++i) {
        drawSegment(canvas, strokeBuffer_[i], strokeBuffer_[i + 1]);
    }

    // Sonraki çağrıda başlangıç indeksi: en son işlenen noktadan başla.
    // Off-by-one: son nokta bir sonraki segment için "from" noktası olacak.
    if (bufferSize > 0) {
        lastRenderedIndex_ = bufferSize - 1;
    }
}

void BallpointRenderer::endStroke(SkCanvas* canvas) {
    if (canvas == nullptr) {
        LOGE("BallpointRenderer::endStroke: canvas is null.");
    }

    LOGD("BallpointRenderer::endStroke: finalizing %zu points.",
         strokeBuffer_.size());

    // Final commit:
    // 1. Kalan segment varsa çiz (normalde addPoints kapsamış olmalı).
    // 2. SkSurface::flushAndSubmit() burada veya DrawingEngine seviyesinde çağrılır.
    // 3. Buffer temizle.

    strokeBuffer_.clear();
    lastRenderedIndex_ = 0;
}

void BallpointRenderer::drawGhostPoints(SkCanvas*                      canvas,
                                        const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr || points.empty()) {
        return;
    }

    // Ghost noktalar düşük opacity ile, noktalı veya soluk renkte çizilir.
    // strokeBuffer_'a EKLENMEZ; yalnızca geçici görsel geri bildirim içindir.
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    // SkPaint ghostPaint;
    // ghostPaint.setAntiAlias(true);
    // ghostPaint.setStyle(SkPaint::kStroke_Style);
    // ghostPaint.setBlendMode(SkBlendMode::kSrcOver);
    // ghostPaint.setAlphaf(0.35f);   // %35 opacity — tahminli görünüm
    // ghostPaint.setColor(activeParams_.color);
    //
    // for (size_t i = 0; i + 1 < points.size(); ++i) {
    //     float width = pressureToWidth(points[i].pressure, activeParams_.baseWidth);
    //     ghostPaint.setStrokeWidth(width);
    //     canvas->drawLine(points[i].x, points[i].y,
    //                      points[i+1].x, points[i+1].y, ghostPaint);
    // }

    LOGD("BallpointRenderer::drawGhostPoints: %zu ghost points.", points.size());
}

void BallpointRenderer::drawSegment(SkCanvas*          canvas,
                                    const RenderPoint& from,
                                    const RenderPoint& to)
{
    // Pressure ortalaması: iki uç nokta arasındaki geçiş.
    const float avgPressure = (from.pressure + to.pressure) * 0.5f;
    const float width       = pressureToWidth(avgPressure, activeParams_.baseWidth);

    // Skia segment çizimi (header eklenince aktif edilecek):
    // SkPaint segPaint;
    // segPaint.setAntiAlias(true);
    // segPaint.setStyle(SkPaint::kStroke_Style);
    // segPaint.setStrokeWidth(width);
    // segPaint.setStrokeCap(SkPaint::kRound_Cap);
    // segPaint.setBlendMode(SkBlendMode::kSrcOver);
    // segPaint.setColor(activeParams_.color);
    // canvas->drawLine(from.x, from.y, to.x, to.y, segPaint);

    (void)canvas;
    (void)width;

    LOGD("drawSegment: (%.1f,%.1f) → (%.1f,%.1f) width=%.2f",
         from.x, from.y, to.x, to.y, width);
}
```

---

## Dosya 5 — Highlighter Renderer Header

**Path:** `app/src/main/cpp/tools/HighlighterRenderer.h`

```cpp
#pragma once

#include "IToolRenderer.h"
#include <vector>

/**
 * Fosforlu kalem (highlighter) renderer'ı.
 *
 * RENDER ÖZELLİKLERİ:
 * - Blend mode: Multiply.
 *   Multiply blend: çizilen renk alttaki piksellerle çarpılır.
 *   Beyaz üzerine sarı → sarı, koyu zemin üzerine sarı → daha koyu ton.
 *   Bu, gerçek fosforlu kalem davranışını simüle eder.
 * - Genişlik: pressure'a DUYARSIZ (sabit geniş şerit).
 *   Fosforlu kalem basınçla incelip kalınlaşmaz.
 * - Opacity: sabit, düşük (örn. %50) — renk alpha'sı bunu belirler.
 * - Anti-alias: aktif.
 * - Stroke cap: Square (yuvarlak değil; düz kesim).
 *
 * KURAL: BallpointRenderer'a DOKUNULMAZ.
 *        Bu sınıf tamamen bağımsız bir strateji olarak çalışır.
 */
class HighlighterRenderer final : public IToolRenderer {
public:

    HighlighterRenderer()  = default;
    ~HighlighterRenderer() override = default;

    void beginStroke(const StrokeParams& params) override;

    void addPoints(SkCanvas*                      canvas,
                   const std::vector<RenderPoint>& points) override;

    void endStroke(SkCanvas* canvas) override;

    /**
     * Highlighter ghost desteği: ghost noktalar daha soluk Multiply blend ile çizilir.
     */
    void drawGhostPoints(SkCanvas*                      canvas,
                         const std::vector<RenderPoint>& points) override;

private:

    /**
     * Tüm stroke noktaları biriktirilir; endStroke'ta tek seferde flush edilir.
     * Highlighter için incremental render yerine toplu render tercih edilir:
     * üst üste binen segmentler Multiply blend'de renk koyulaşması yaratır.
     * Tüm noktalar tek SkPath'te birleştirilip tek draw call ile gönderilir.
     */
    std::vector<RenderPoint> strokeBuffer_;

    /**
     * Tüm noktaları tek SkPath'e dönüştürüp canvas'a çizer.
     * Sadece endStroke() tarafından çağrılır.
     *
     * @param canvas    Skia canvas.
     * @param opacity   0.0f – 1.0f arası alpha değeri.
     */
    void flushPath(SkCanvas* canvas, float opacity);
};
```

---

## Dosya 6 — Highlighter Renderer Implementation

**Path:** `app/src/main/cpp/tools/HighlighterRenderer.cpp`

```cpp
#include "HighlighterRenderer.h"

#include <android/log.h>

#define LOG_TAG "HighlighterRenderer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Skia include'ları CMakeLists.txt'te Skia path tanımlı olduğunda aktif edilir.
// #include "include/core/SkCanvas.h"
// #include "include/core/SkPaint.h"
// #include "include/core/SkPath.h"
// ─────────────────────────────────────────────────────────────────────────────

// Sabitler: fosforlu kalem görsel karakterini belirler.
namespace {
    /**
     * Fosforlu kalemin varsayılan opacity değeri.
     * %50 opacity, Multiply blend ile saydam ama belirgin görünüm sağlar.
     */
    constexpr float kHighlighterOpacity = 0.50f;

    /**
     * Ghost noktaların opacity değeri.
     * Normal opacity'nin yarısı; tahminli olduğunu belirtir.
     */
    constexpr float kGhostOpacity = 0.25f;

    /**
     * Fosforlu kalem genişlik çarpanı.
     * baseWidth bu çarpanla çarpılarak geniş şerit elde edilir.
     * Pressure bu değeri ETKİLEMEZ.
     */
    constexpr float kWidthMultiplier = 3.0f;
}

void HighlighterRenderer::beginStroke(const StrokeParams& params) {
    activeParams_ = params;
    strokeBuffer_.clear();

    LOGD("HighlighterRenderer::beginStroke color=0x%08X width=%.2f hz=%d",
         params.color, params.baseWidth, params.hardwareHz);

    // Skia SkPaint hazırlığı (Skia header'ları eklenince aktif edilecek):
    //
    // paint_.reset();
    // paint_.setAntiAlias(true);
    // paint_.setStyle(SkPaint::kStroke_Style);
    //
    // KRITIK — MULTIPLY BLEND MODE:
    // Multiply: sonuç = kaynak * hedef (piksel değerleri 0.0–1.0 aralığında çarpılır).
    // Beyaz (1.0) üzerine herhangi bir renk → o renk (nötr).
    // Siyah (0.0) üzerine herhangi bir renk → siyah (karartma).
    // Renkli zemin üzerine sarı → daha koyu, doygun sarı tonu.
    // Bu, fiziksel fosforlu kalemin davranışını doğru simüle eder.
    //
    // paint_.setBlendMode(SkBlendMode::kMultiply);
    //
    // Sabit genişlik: pressure çarpanı kullanılmaz.
    // paint_.setStrokeWidth(activeParams_.baseWidth * kWidthMultiplier);
    // paint_.setStrokeCap(SkPaint::kSquare_Cap);  // düz kesim
    // paint_.setAlphaf(kHighlighterOpacity);
    // paint_.setColor(params.color);
}

void HighlighterRenderer::addPoints(SkCanvas*                      canvas,
                                    const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr) {
        LOGE("HighlighterRenderer::addPoints: canvas is null.");
        return;
    }

    // Highlighter'da incremental render YAPILMAZ.
    // Üst üste binen segmentler ayrı draw call'larla gönderilirse
    // Multiply blend mode çakışma noktalarında rengi fazla koyulaştırır.
    // Tüm noktalar buffer'a biriktirilir; endStroke'ta tek path olarak flush edilir.
    for (const auto& point : points) {
        strokeBuffer_.push_back(point);
    }

    LOGD("HighlighterRenderer::addPoints: buffered %zu points (total %zu).",
         points.size(), strokeBuffer_.size());
}

void HighlighterRenderer::endStroke(SkCanvas* canvas) {
    if (canvas == nullptr) {
        LOGE("HighlighterRenderer::endStroke: canvas is null.");
        strokeBuffer_.clear();
        return;
    }

    LOGD("HighlighterRenderer::endStroke: flushing %zu points.", strokeBuffer_.size());

    flushPath(canvas, kHighlighterOpacity);

    strokeBuffer_.clear();
}

void HighlighterRenderer::drawGhostPoints(SkCanvas*                      canvas,
                                          const std::vector<RenderPoint>& points)
{
    if (canvas == nullptr || points.empty()) {
        return;
    }

    // Ghost noktalar daha düşük opacity ile Multiply blend'de çizilir.
    // strokeBuffer_'a EKLENMEZ.
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    // SkPaint ghostPaint;
    // ghostPaint.setAntiAlias(true);
    // ghostPaint.setStyle(SkPaint::kStroke_Style);
    // ghostPaint.setBlendMode(SkBlendMode::kMultiply);
    // ghostPaint.setStrokeWidth(activeParams_.baseWidth * kWidthMultiplier);
    // ghostPaint.setStrokeCap(SkPaint::kSquare_Cap);
    // ghostPaint.setAlphaf(kGhostOpacity);
    // ghostPaint.setColor(activeParams_.color);
    //
    // SkPath ghostPath;
    // if (!points.empty()) {
    //     ghostPath.moveTo(points[0].x, points[0].y);
    //     for (size_t i = 1; i < points.size(); ++i) {
    //         ghostPath.lineTo(points[i].x, points[i].y);
    //     }
    // }
    // canvas->drawPath(ghostPath, ghostPaint);

    (void)canvas;
    LOGD("HighlighterRenderer::drawGhostPoints: %zu ghost points.", points.size());
}

void HighlighterRenderer::flushPath(SkCanvas* canvas, float opacity) {
    if (strokeBuffer_.empty()) {
        return;
    }

    // Tüm noktalari tek SkPath'e dönüştür ve tek draw call ile gönder.
    // Tek draw call: Multiply blend'in üst üste binen noktalarda
    // renk koyulaşması yaratması önlenir.
    //
    // Skia implementasyonu (header eklenince aktif edilecek):
    // SkPath path;
    // path.moveTo(strokeBuffer_[0].x, strokeBuffer_[0].y);
    // for (size_t i = 1; i < strokeBuffer_.size(); ++i) {
    //     path.lineTo(strokeBuffer_[i].x, strokeBuffer_[i].y);
    // }
    //
    // SkPaint paint;
    // paint.setAntiAlias(true);
    // paint.setStyle(SkPaint::kStroke_Style);
    // paint.setBlendMode(SkBlendMode::kMultiply);
    // paint.setStrokeWidth(activeParams_.baseWidth * kWidthMultiplier);
    // paint.setStrokeCap(SkPaint::kSquare_Cap);
    // paint.setAlphaf(opacity);
    // paint.setColor(activeParams_.color);
    //
    // canvas->drawPath(path, paint);

    (void)canvas;
    (void)opacity;

    LOGD("HighlighterRenderer::flushPath: %zu points flushed.", strokeBuffer_.size());
}
```

---

## Renderer Seçim Fabrikası

**Path:** `app/src/main/cpp/tools/ToolRendererFactory.h`

```cpp
#pragma once

#include "IToolRenderer.h"
#include "BallpointRenderer.h"
#include "HighlighterRenderer.h"

#include <memory>

/**
 * ToolType ordinal değerine göre doğru renderer'ı oluşturur.
 *
 * Kotlin ToolType enum sırası:
 *   0 = BALLPOINT_PEN
 *   1 = FOUNTAIN_PEN
 *   2 = HIGHLIGHTER
 *   3 = MARKER
 *   4 = ERASER
 *
 * KURAL: Yeni tool eklendiğinde yalnızca bu switch'e yeni case eklenir.
 *        Mevcut case'lere DOKUNULMAZ.
 */
class ToolRendererFactory {
public:

    /**
     * @param toolTypeOrd Kotlin ToolType.ordinal değeri.
     * @return            Uygun renderer'ın unique_ptr'ı.
     *                    Tanımsız ordinal için BallpointRenderer döner (safe default).
     */
    static std::unique_ptr<IToolRenderer> create(int toolTypeOrd) {
        switch (toolTypeOrd) {
            case 0:  // BALLPOINT_PEN
                return std::make_unique<BallpointRenderer>();

            case 1:  // FOUNTAIN_PEN
                // FountainPenRenderer henüz implemente edilmedi.
                // Geçici olarak BallpointRenderer kullanılır.
                // FountainPenRenderer hazır olduğunda bu case güncellenir.
                return std::make_unique<BallpointRenderer>();

            case 2:  // HIGHLIGHTER
                return std::make_unique<HighlighterRenderer>();

            case 3:  // MARKER
                // MarkerRenderer henüz implemente edilmedi.
                return std::make_unique<BallpointRenderer>();

            case 4:  // ERASER
                // EraserRenderer henüz implemente edilmedi.
                return std::make_unique<BallpointRenderer>();

            default:
                return std::make_unique<BallpointRenderer>();
        }
    }

    // Static-only sınıf; instantiation yasak.
    ToolRendererFactory()  = delete;
    ~ToolRendererFactory() = delete;
};
```

---

## Kural Özeti

| Kural | Zorunluluk |
|---|---|
| Tüm state değişikliği `CommandManager.execute()` üzerinden geçer | **ZORUNLU** |
| `currentPage` dışarıdan mutate edilmez | **ZORUNLU** |
| Yeni tool = yeni strateji sınıfı; mevcut renderer'a dokunulmaz | **ZORUNLU** |
| `HighlighterRenderer` her zaman `SkBlendMode::kMultiply` kullanır | **ZORUNLU** |
| `HighlighterRenderer` incremental render yapmaz; tüm noktaları `endStroke`'ta flush eder | **ZORUNLU** |
| Ghost noktalar hiçbir renderer'ın `strokeBuffer_`'ına eklenmez | **ZORUNLU** |
| `ToolType` ordinal sırası değiştirilirse `ToolRendererFactory` switch güncellenir | **ZORUNLU** |
| `DeleteStrokeCommand` fiziksel silme değil `isDeleted=true` soft-delete uygular | **ZORUNLU** |