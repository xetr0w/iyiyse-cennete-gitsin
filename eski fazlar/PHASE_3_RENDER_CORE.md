# PHASE_3_RENDER_CORE.md

## Amaç

Bu doküman, Kotlin input katmanından gelen `StylusPoint` verisini JNI köprüsü üzerinden C++ motoruna aktaran ve Skia tabanlı GPU render iskeletini kuran çekirdek render katmanını tanımlar.

### Mimari Pozisyon

```
DynamicInputSampler (Kotlin)
        │
        ▼
NativeDrawingEngine.kt   ← JNI Wrapper (bu dosya)
        │   JNI sınırı
        ▼
drawing_engine_jni.cpp   ← JNI Implementation (bu dosya)
        │
        ▼
DrawingEngine (C++)      ← Skia / GPU render çekirdeği
```

---

## Dosya 1 — Kotlin JNI Wrapper

**Path:** `app/src/main/java/com/yourdomain/notes/engine/NativeDrawingEngine.kt`

```kotlin
package com.yourdomain.notes.engine

import android.view.Surface

/**
 * C++ DrawingEngine'e tek erişim noktası.
 *
 * KURALLAR:
 * - Bu sınıftaki hiçbir fonksiyon UI thread'de hesaplama yapmaz;
 *   yalnızca JNI çağrısı yapar.
 * - [initEngine] çağrılmadan diğer fonksiyonlar çağrılmamalıdır.
 * - [destroyEngine] çağrıldıktan sonra bu nesne kullanılamaz.
 * - [addPoints] parametrelerindeki FloatArray ve LongArray dizileri
 *   şu düzende paketlenmiş olmalıdır:
 *     floatData  → [x0, y0, pressure0, x1, y1, pressure1, ...]
 *     timestamps → [ts0, ts1, ...]   (nanosaniye cinsinden)
 *   Her iki dizinin eleman sayısı tutarlı olmalıdır:
 *     floatData.size == timestamps.size * 3
 *
 * JNI İMZA UYARISI:
 * Bu dosyadaki paket adı veya sınıf adı değiştirilirse,
 * drawing_engine_jni.cpp içindeki tüm fonksiyon imzaları
 * manuel olarak güncellenmelidir. İmza uyumsuzluğu runtime'da
 * UnsatisfiedLinkError fırlatır.
 */
object NativeDrawingEngine {

    init {
        // Kütüphane adı CMakeLists.txt içindeki add_library() adıyla
        // birebir eşleşmelidir.
        System.loadLibrary("drawing_engine")
    }

    // ─────────────────────────────────────────────────────────────────────────
    // YAŞAM DÖNGÜSÜ
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * C++ motorunu başlatır ve verilen [Surface]'e bağlar.
     * SurfaceHolder.Callback.surfaceCreated() içinden çağrılmalıdır.
     *
     * @param surface Skia'nın render çıktısını yazacağı Android Surface.
     */
    external fun initEngine(surface: Surface)

    /**
     * C++ motorunu durdurur, tüm Skia kaynaklarını serbest bırakır.
     * SurfaceHolder.Callback.surfaceDestroyed() içinden çağrılmalıdır.
     * Bu çağrıdan sonra diğer fonksiyonlar çağrılmamalıdır.
     */
    external fun destroyEngine()

    /**
     * Donanım örnekleme frekansını C++ motoruna iletir.
     * DynamicInputSampler.detectedHardwareHz değeri hazır olduğunda çağrılır.
     *
     * @param hz Tespit edilen donanım Hz değeri (60, 120 veya 240).
     */
    external fun setHardwareHz(hz: Int)

    // ─────────────────────────────────────────────────────────────────────────
    // STROKE YAŞAM DÖNGÜSÜ
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Yeni bir stroke başlatır.
     * ACTION_DOWN alındığında çağrılmalıdır.
     *
     * @param strokeId    Stroke'un UUID string'i (CRDT uyumluluğu için zorunlu).
     * @param toolTypeOrd ToolType.ordinal değeri.
     * @param color       ARGB int renk değeri.
     * @param baseWidth   Piksel cinsinden temel kalem genişliği.
     */
    external fun beginStroke(
        strokeId: String,
        toolTypeOrd: Int,
        color: Int,
        baseWidth: Float
    )

    /**
     * Mevcut aktif stroke'a nokta batch'i ekler.
     * ACTION_MOVE her alındığında çağrılmalıdır.
     *
     * @param floatData  Paketlenmiş [x, y, pressure] dizisi.
     *                   Düzen: [x0, y0, p0, x1, y1, p1, ...]
     *                   floatData.size % 3 == 0 olmalıdır.
     * @param timestamps Her noktanın nanosaniye cinsinden timestamp'i.
     *                   timestamps.size == floatData.size / 3 olmalıdır.
     */
    external fun addPoints(floatData: FloatArray, timestamps: LongArray)

    /**
     * Aktif stroke'u sonlandırır ve GPU'ya commit eder.
     * ACTION_UP veya ACTION_CANCEL alındığında çağrılmalıdır.
     */
    external fun endStroke()

    // ─────────────────────────────────────────────────────────────────────────
    // GHOST (TAHMİNLİ) NOKTALAR
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Tahminli (ghost) noktaları C++ motoruna iletir.
     * Bu noktalar geçici görsel katmana çizilir; kalıcı stroke verisine eklenmez.
     * Her yeni gerçek nokta geldiğinde C++ bu geçici katmanı temizler.
     *
     * @param floatData  [x, y, pressure] düzeninde paketlenmiş ghost noktalar.
     * @param timestamps Nanosaniye cinsinden timestamp'ler.
     */
    external fun updateGhostPoints(floatData: FloatArray, timestamps: LongArray)

    /**
     * Ghost (tahminli) görsel katmanını temizler.
     * Gerçek nokta commit edildiğinde C++ katmanı bu geçici çizimi siler.
     */
    external fun clearGhostPoints()
}
```

---

## Dosya 2 — C++ JNI Implementation

**Path:** `app/src/main/cpp/drawing_engine_jni.cpp`

```cpp
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <memory>
#include <vector>
#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// LOGGING
// ─────────────────────────────────────────────────────────────────────────────

#define LOG_TAG "DrawingEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// VERI YAPILARI
// ─────────────────────────────────────────────────────────────────────────────

/**
 * C++ tarafındaki tek bir dokunma noktası.
 * Kotlin StylusPoint ile birebir eşleşir.
 * timestamp KESİNLİKLE nanosaniye cinsinden olmalıdır.
 */
struct StylusPoint {
    float    x;
    float    y;
    float    pressure;
    int64_t  timestamp;  // nanoseconds
};

/**
 * Tek bir stroke'un C++ taraftaki temsili.
 * Tüm alanlar beginStroke() çağrısında set edilir,
 * noktalar addPoints() ile biriktirilir.
 */
struct StrokeData {
    std::string          strokeId;
    int                  toolTypeOrd;
    int                  color;
    float                baseWidth;
    std::vector<StylusPoint> points;
};

// ─────────────────────────────────────────────────────────────────────────────
// DRAWING ENGINE — C++ ÇEKİRDEK SINIFI
// RAII: Tüm kaynaklar destructor'da serbest bırakılır.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Tüm render durumunu ve Skia kaynaklarını yöneten merkezi sınıf.
 *
 * YAŞAM DÖNGÜSÜ:
 *   DrawingEngine engine(window);   → initEngine()
 *   engine.beginStroke(...)         → ACTION_DOWN
 *   engine.addPoints(...)           → ACTION_MOVE (tekrar tekrar)
 *   engine.endStroke()              → ACTION_UP
 *   ~DrawingEngine()                → destroyEngine() (RAII)
 *
 * BELLEK YÖNETİMİ:
 *   - ANativeWindow referansı constructor'da acquire edilir,
 *     destructor'da release edilir.
 *   - Skia nesneleri (SkSurface, GrDirectContext vb.) ileride
 *     std::unique_ptr ile sarmalanacaktır.
 *   - activeStroke_, ghostPoints_ heap'te değil stack'te tutulur;
 *     std::vector RAII ile kendi belleğini yönetir.
 */
class DrawingEngine {
public:

    /**
     * @param window initEngine'den gelen ANativeWindow pointer'ı.
     *               Constructor ANativeWindow_acquire çağırır;
     *               destructor ANativeWindow_release çağırır.
     */
    explicit DrawingEngine(ANativeWindow* window)
        : window_(window),
          hardwareHz_(60),
          strokeActive_(false)
    {
        if (window_ != nullptr) {
            ANativeWindow_acquire(window_);
            LOGI("DrawingEngine created. Window: %p", window_);
            initSkia();
        } else {
            LOGE("DrawingEngine created with null window.");
        }
    }

    /**
     * Destructor — tüm Skia kaynakları ve ANativeWindow burada serbest bırakılır.
     * delete veya manuel release çağrısına gerek yoktur.
     */
    ~DrawingEngine() {
        destroySkia();
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
            LOGI("DrawingEngine destroyed. ANativeWindow released.");
        }
    }

    // Kopyalamayı yasakla: ANativeWindow sahipliği tekil olmalıdır.
    DrawingEngine(const DrawingEngine&)            = delete;
    DrawingEngine& operator=(const DrawingEngine&) = delete;

    // Move semantics: ileride gerekirse eklenebilir.
    DrawingEngine(DrawingEngine&&)                 = delete;
    DrawingEngine& operator=(DrawingEngine&&)      = delete;

    // ─────────────────────────────────────────────────────────────────────
    // KONFIGÜRASYON
    // ─────────────────────────────────────────────────────────────────────

    /**
     * Donanım örnekleme frekansını ayarlar.
     * Bu değer Catmull-Rom / Bezier interpolasyon adım sayısını etkiler.
     *
     * @param hz 60, 120 veya 240.
     */
    void setHardwareHz(int hz) {
        hardwareHz_ = hz;
        LOGD("Hardware Hz set to: %d", hardwareHz_);
    }

    // ─────────────────────────────────────────────────────────────────────
    // STROKE YAŞAM DÖNGÜSÜ
    // ─────────────────────────────────────────────────────────────────────

    /**
     * Yeni bir stroke başlatır. Önceki tamamlanmamış stroke varsa
     * güvenli şekilde sonlandırılır (drop edilir, log yazılır).
     */
    void beginStroke(const std::string& strokeId,
                     int                toolTypeOrd,
                     int                color,
                     float              baseWidth)
    {
        if (strokeActive_) {
            LOGE("beginStroke called while stroke active. Dropping previous stroke: %s",
                 activeStroke_.strokeId.c_str());
        }

        activeStroke_             = StrokeData{};
        activeStroke_.strokeId    = strokeId;
        activeStroke_.toolTypeOrd = toolTypeOrd;
        activeStroke_.color       = color;
        activeStroke_.baseWidth   = baseWidth;
        activeStroke_.points.clear();
        strokeActive_             = true;

        LOGD("beginStroke: id=%s tool=%d color=0x%08X width=%.2f",
             strokeId.c_str(), toolTypeOrd, color, baseWidth);
    }

    /**
     * Aktif stroke'a nokta batch'i ekler.
     * Filtering ve Bezier interpolasyon bu fonksiyonda gerçekleşir.
     *
     * @param points Paketlenmiş StylusPoint vektörü.
     *               Kaynak: JNI katmanında FloatArray + LongArray'den dönüştürülmüş.
     */
    void addPoints(const std::vector<StylusPoint>& points) {
        if (!strokeActive_) {
            LOGE("addPoints called without active stroke. Ignoring %zu points.",
                 points.size());
            return;
        }

        for (const auto& point : points) {
            activeStroke_.points.push_back(point);
        }

        // Performans kritik işlemler burada yapılır:
        // 1. Kalman / Savitzky-Golay filtering
        // 2. Catmull-Rom Bezier interpolation
        // 3. Variable-width triangulation (pressure → width mapping)
        // 4. Skia SkPath güncelleme ve incremental GPU draw call
        renderIncrementalStroke(points);
    }

    /**
     * Aktif stroke'u sonlandırır, GPU'ya final commit yapar
     * ve dahili stroke tamponunu temizler.
     */
    void endStroke() {
        if (!strokeActive_) {
            LOGE("endStroke called without active stroke. No-op.");
            return;
        }

        LOGD("endStroke: id=%s totalPoints=%zu",
             activeStroke_.strokeId.c_str(),
             activeStroke_.points.size());

        commitStrokeToGpu();

        activeStroke_ = StrokeData{};
        strokeActive_ = false;
    }

    // ─────────────────────────────────────────────────────────────────────
    // GHOST NOKTALAR
    // ─────────────────────────────────────────────────────────────────────

    /**
     * Ghost (tahminli) noktaları geçici görsel katmana çizer.
     * Bu noktalar activeStroke_.points'e EKLENMEZ.
     * Bir sonraki gerçek nokta geldiğinde clearGhostPoints() çağrılır.
     */
    void updateGhostPoints(const std::vector<StylusPoint>& ghostPoints) {
        ghostPoints_ = ghostPoints;
        renderGhostLayer();
    }

    /**
     * Ghost görsel katmanını temizler.
     * Skia geçici layer'ı invalidate eder; bir sonraki frame'de
     * sadece gerçek stroke'lar görünür.
     */
    void clearGhostPoints() {
        ghostPoints_.clear();
        invalidateGhostLayer();
    }

private:

    // ─────────────────────────────────────────────────────────────────────
    // ÖZELLİKLER
    // ─────────────────────────────────────────────────────────────────────

    ANativeWindow*           window_;
    int                      hardwareHz_;
    bool                     strokeActive_;
    StrokeData               activeStroke_;
    std::vector<StylusPoint> ghostPoints_;

    // Skia nesneleri ileride şu türlerle tanımlanacak:
    //   std::unique_ptr<GrDirectContext> grContext_;
    //   std::unique_ptr<SkSurface>       skSurface_;
    // Şimdilik void* placeholder olarak tutulur; Skia bağımlılığı
    // CMakeLists.txt'e eklendikten sonra tip güvenli hale getirilecek.
    void* grContext_  = nullptr;
    void* skSurface_  = nullptr;

    // ─────────────────────────────────────────────────────────────────────
    // SKIA YAŞAM DÖNGÜSÜ
    // ─────────────────────────────────────────────────────────────────────

    /**
     * Skia GrDirectContext ve SkSurface'i başlatır.
     * ANativeWindow'dan EGLSurface oluşturulur; Skia GPU backend bağlanır.
     *
     * BAĞIMLILIK: CMakeLists.txt'te Skia static/shared kütüphanesi
     * ve include path'leri tanımlı olmalıdır.
     */
    void initSkia() {
        LOGI("initSkia: Skia GPU context initialization placeholder.");
        // TODO: GrDirectContext::MakeGL() veya MakeVulkan() çağrısı
        // TODO: SkSurface::MakeFromBackendRenderTarget() çağrısı
    }

    /**
     * Tüm Skia nesnelerini serbest bırakır.
     * Destructor'dan çağrılır; unique_ptr kullanıldığında otomatikleşir.
     */
    void destroySkia() {
        LOGI("destroySkia: Releasing Skia resources.");
        // unique_ptr kullanıldığında bu gövde boş kalabilir.
        // Şimdilik manual cleanup için yer tutucu.
        grContext_ = nullptr;
        skSurface_ = nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────
    // RENDER FONKSİYONLARI
    // ─────────────────────────────────────────────────────────────────────

    /**
     * Yeni gelen nokta batch'ini Skia SkPath'e ekler ve incremental draw yapar.
     * UI thread'i bloklamaz; render thread'den çağrılması beklenir.
     *
     * @param newPoints Bu çağrıda eklenen yeni noktalar.
     *                  activeStroke_.points zaten güncellenmiş durumdadır.
     */
    void renderIncrementalStroke(const std::vector<StylusPoint>& newPoints) {
        if (newPoints.empty()) return;

        // Aşamalar (ileride implemente edilecek):
        // 1. Kalman filtresi: noktaları gürültüden arındır.
        // 2. Catmull-Rom spline: noktalar arası smooth interpolasyon.
        // 3. Pressure → width mapping: her segment için genişlik hesapla.
        // 4. Triangulation: variable-width şeridi üçgenlere böl.
        // 5. Skia SkCanvas::drawPath() veya SkCanvas::drawVertices() ile GPU'ya gönder.

        LOGD("renderIncrementalStroke: %zu new points for stroke %s",
             newPoints.size(), activeStroke_.strokeId.c_str());
    }

    /**
     * Stroke tamamlandığında tüm noktaları final Skia path'e işler
     * ve GPU frame buffer'a commit eder.
     */
    void commitStrokeToGpu() {
        LOGD("commitStrokeToGpu: stroke %s committed with %zu points.",
             activeStroke_.strokeId.c_str(),
             activeStroke_.points.size());

        // Aşamalar:
        // 1. Final SkPath oluştur (tüm interpolated noktalar).
        // 2. SkPaint ayarla (renk, anti-alias, blend mode).
        // 3. SkCanvas::drawPath() ile kalıcı katmana çiz.
        // 4. SkSurface::flushAndSubmit() ile GPU'ya gönder.
    }

    /**
     * Ghost noktaları geçici Skia layer'ına çizer.
     * Bu layer her frame'de clearGhostPoints() ile silinir.
     */
    void renderGhostLayer() {
        if (ghostPoints_.empty()) return;

        LOGD("renderGhostLayer: %zu ghost points.", ghostPoints_.size());

        // Aşamalar:
        // 1. Geçici SkSurface veya saveLayer() ile izole katman aç.
        // 2. Ghost noktaları düşük opacity ile çiz.
        // 3. Layer'ı ana canvas'a composite et.
    }

    /**
     * Ghost layer'ını invalidate eder.
     * Skia saveLayer kullanılıyorsa restore() çağrılır.
     */
    void invalidateGhostLayer() {
        LOGD("invalidateGhostLayer: ghost layer cleared.");
        // Geçici katmanı temizle; bir sonraki frame sadece gerçek stroke'ları gösterir.
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// GLOBAL ENGINE INSTANCE
// std::unique_ptr: new/delete yok, RAII garantisi tam.
// JNI fonksiyonları bu pointer üzerinden engine'e erişir.
// Thread safety: JNI çağrıları tek thread'den geldiği varsayılır.
// Çok thread'li erişim gerekirse mutex eklenmelidir.
// ─────────────────────────────────────────────────────────────────────────────

static std::unique_ptr<DrawingEngine> gEngine;

// ─────────────────────────────────────────────────────────────────────────────
// YARDIMCI FONKSİYONLAR — JNI DİZİ DÖNÜŞÜMÜ
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin'den gelen jfloatArray ve jlongArray'i std::vector<StylusPoint>'e dönüştürür.
 *
 * BELLEK YÖNETİMİ — KRİTİK:
 * GetFloatArrayElements / GetLongArrayElements çağrısından sonra
 * ReleaseFloatArrayElements / ReleaseLongArrayElements MUTLAKA çağrılmalıdır.
 * Bu fonksiyon erken return durumlarında bile Release'i garantilemek için
 * goto yerine RAII benzeri bir yapı kullanır.
 *
 * PAKET DÜZENİ:
 *   floatData : [x0, y0, p0, x1, y1, p1, ...]
 *   timestamps: [ts0, ts1, ...]
 *   pointCount = timestamps dizisinin uzunluğu
 *   floatData.length == pointCount * 3 olmalıdır.
 *
 * @param env         JNI ortam pointer'ı.
 * @param floatArray  Kotlin'den gelen x/y/pressure dizisi.
 * @param longArray   Kotlin'den gelen nanosaniye timestamp dizisi.
 * @return            Dönüştürülmüş StylusPoint vektörü.
 *                    Hata durumunda boş vektör döner.
 */
static std::vector<StylusPoint> convertArraysToPoints(JNIEnv*     env,
                                                       jfloatArray floatArray,
                                                       jlongArray  longArray)
{
    std::vector<StylusPoint> result;

    if (floatArray == nullptr || longArray == nullptr) {
        LOGE("convertArraysToPoints: null array received.");
        return result;
    }

    jsize floatLen = env->GetArrayLength(floatArray);
    jsize longLen  = env->GetArrayLength(longArray);

    // Dizi uzunluğu tutarlılık kontrolü.
    if (floatLen == 0 || longLen == 0) {
        LOGE("convertArraysToPoints: empty array received.");
        return result;
    }

    if (floatLen != longLen * 3) {
        LOGE("convertArraysToPoints: size mismatch. floatLen=%d, longLen=%d",
             floatLen, longLen);
        return result;
    }

    // ── Float array pinning ───────────────────────────────────────────────────
    // JNI_ABORT: C++ tarafında bu dizi değiştirilmediğinden
    // Kotlin heap'e geri kopyalama yapılmaz. Performans optimizasyonu.
    jboolean floatIsCopy = JNI_FALSE;
    jfloat* floatData = env->GetFloatArrayElements(floatArray, &floatIsCopy);

    if (floatData == nullptr) {
        LOGE("convertArraysToPoints: GetFloatArrayElements returned null.");
        return result;
    }

    // ── Long array pinning ────────────────────────────────────────────────────
    jboolean longIsCopy = JNI_FALSE;
    jlong* longData = env->GetLongArrayElements(longArray, &longIsCopy);

    if (longData == nullptr) {
        LOGE("convertArraysToPoints: GetLongArrayElements returned null.");
        // floatData zaten pin edildi; Release ZORUNLU, aksi halde memory leak.
        env->ReleaseFloatArrayElements(floatArray, floatData, JNI_ABORT);
        return result;
    }

    // ── Dönüşüm ──────────────────────────────────────────────────────────────
    result.reserve(static_cast<size_t>(longLen));

    for (jsize i = 0; i < longLen; ++i) {
        jsize baseIndex = i * 3;
        StylusPoint point{};
        point.x         = floatData[baseIndex + 0];
        point.y         = floatData[baseIndex + 1];
        point.pressure  = floatData[baseIndex + 2];
        point.timestamp = static_cast<int64_t>(longData[i]);  // nanoseconds
        result.push_back(point);
    }

    // ── Release — HER İKİ DİZİ ZORUNLU ──────────────────────────────────────
    // JNI_ABORT: C++ bu dizileri değiştirmedi; Kotlin heap'e geri yazma yok.
    // Bu çağrılar yapılmazsa JVM, pinned memory'i asla serbest bırakmaz.
    env->ReleaseFloatArrayElements(floatArray, floatData, JNI_ABORT);
    env->ReleaseLongArrayElements(longArray,  longData,  JNI_ABORT);

    LOGD("convertArraysToPoints: converted %d points.", longLen);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// JNI EXPORT FONKSİYONLARI
//
// İMZA KURALI:
// Java_<paket_alt_çizgili>_<sınıf>_<fonksiyon>
// Paket: com.yourdomain.notes.engine → com_yourdomain_notes_engine
// Sınıf: NativeDrawingEngine
//
// Bu imzalar NativeDrawingEngine.kt içindeki `external fun` bildirimleriyle
// birebir eşleşmelidir. Paket veya sınıf adı değişirse imzalar güncellenmeli.
// ─────────────────────────────────────────────────────────────────────────────

extern "C" {

// ─────────────────────────────────────────────────────────────────────────────
// initEngine
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun initEngine(surface: Surface)
 *
 * jobject surface → ANativeWindow_fromSurface → DrawingEngine constructor.
 * ANativeWindow referansı DrawingEngine içinde acquire/release ile yönetilir.
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_initEngine(
        JNIEnv*  env,
        jobject  /* thiz */,
        jobject  surface)
{
    if (surface == nullptr) {
        LOGE("initEngine: surface is null.");
        return;
    }

    if (gEngine != nullptr) {
        LOGE("initEngine: engine already initialized. Destroying previous instance.");
        gEngine.reset();
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        LOGE("initEngine: ANativeWindow_fromSurface failed.");
        return;
    }

    // unique_ptr: heap allocation, RAII ile yönetilir.
    // make_unique kullanılır; new/delete yoktur.
    gEngine = std::make_unique<DrawingEngine>(window);

    // DrawingEngine constructor ANativeWindow_acquire çağırdı;
    // buradaki local referansı serbest bırakıyoruz.
    ANativeWindow_release(window);

    LOGI("initEngine: DrawingEngine initialized successfully.");
}

// ─────────────────────────────────────────────────────────────────────────────
// destroyEngine
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun destroyEngine()
 *
 * gEngine.reset() → ~DrawingEngine() → ANativeWindow_release + Skia cleanup.
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_destroyEngine(
        JNIEnv* /* env */,
        jobject /* thiz */)
{
    if (gEngine == nullptr) {
        LOGE("destroyEngine: engine not initialized. No-op.");
        return;
    }

    gEngine.reset();
    LOGI("destroyEngine: DrawingEngine destroyed.");
}

// ─────────────────────────────────────────────────────────────────────────────
// setHardwareHz
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun setHardwareHz(hz: Int)
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_setHardwareHz(
        JNIEnv* /* env */,
        jobject /* thiz */,
        jint    hz)
{
    if (gEngine == nullptr) {
        LOGE("setHardwareHz: engine not initialized.");
        return;
    }

    gEngine->setHardwareHz(static_cast<int>(hz));
}

// ─────────────────────────────────────────────────────────────────────────────
// beginStroke
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun beginStroke(strokeId: String, toolTypeOrd: Int,
 *                                   color: Int, baseWidth: Float)
 *
 * jstring → std::string dönüşümü:
 * GetStringUTFChars / ReleaseStringUTFChars çifti zorunludur.
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_beginStroke(
        JNIEnv*  env,
        jobject  /* thiz */,
        jstring  strokeId,
        jint     toolTypeOrd,
        jint     color,
        jfloat   baseWidth)
{
    if (gEngine == nullptr) {
        LOGE("beginStroke: engine not initialized.");
        return;
    }

    if (strokeId == nullptr) {
        LOGE("beginStroke: strokeId is null.");
        return;
    }

    // jstring → const char* → std::string
    jboolean isCopy = JNI_FALSE;
    const char* strokeIdChars = env->GetStringUTFChars(strokeId, &isCopy);

    if (strokeIdChars == nullptr) {
        LOGE("beginStroke: GetStringUTFChars failed.");
        return;
    }

    std::string strokeIdStr(strokeIdChars);

    // ZORUNLU: GetStringUTFChars sonrası ReleaseStringUTFChars.
    env->ReleaseStringUTFChars(strokeId, strokeIdChars);

    gEngine->beginStroke(strokeIdStr,
                         static_cast<int>(toolTypeOrd),
                         static_cast<int>(color),
                         static_cast<float>(baseWidth));
}

// ─────────────────────────────────────────────────────────────────────────────
// addPoints
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun addPoints(floatData: FloatArray, timestamps: LongArray)
 *
 * Dizi dönüşümü ve Release garantisi convertArraysToPoints() içinde yönetilir.
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_addPoints(
        JNIEnv*     env,
        jobject     /* thiz */,
        jfloatArray floatData,
        jlongArray  timestamps)
{
    if (gEngine == nullptr) {
        LOGE("addPoints: engine not initialized.");
        return;
    }

    std::vector<StylusPoint> points = convertArraysToPoints(env, floatData, timestamps);

    if (points.empty()) {
        LOGD("addPoints: no valid points to add.");
        return;
    }

    gEngine->addPoints(points);
}

// ─────────────────────────────────────────────────────────────────────────────
// endStroke
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun endStroke()
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_endStroke(
        JNIEnv* /* env */,
        jobject /* thiz */)
{
    if (gEngine == nullptr) {
        LOGE("endStroke: engine not initialized.");
        return;
    }

    gEngine->endStroke();
}

// ─────────────────────────────────────────────────────────────────────────────
// updateGhostPoints
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun updateGhostPoints(floatData: FloatArray, timestamps: LongArray)
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_updateGhostPoints(
        JNIEnv*     env,
        jobject     /* thiz */,
        jfloatArray floatData,
        jlongArray  timestamps)
{
    if (gEngine == nullptr) {
        LOGE("updateGhostPoints: engine not initialized.");
        return;
    }

    std::vector<StylusPoint> ghostPoints = convertArraysToPoints(env, floatData, timestamps);

    if (ghostPoints.empty()) {
        LOGD("updateGhostPoints: no valid ghost points.");
        return;
    }

    gEngine->updateGhostPoints(ghostPoints);
}

// ─────────────────────────────────────────────────────────────────────────────
// clearGhostPoints
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Kotlin: external fun clearGhostPoints()
 */
JNIEXPORT void JNICALL
Java_com_yourdomain_notes_engine_NativeDrawingEngine_clearGhostPoints(
        JNIEnv* /* env */,
        jobject /* thiz */)
{
    if (gEngine == nullptr) {
        LOGE("clearGhostPoints: engine not initialized.");
        return;
    }

    gEngine->clearGhostPoints();
}

}  // extern "C"
```

---

## JNI İmza Referans Tablosu

| Kotlin `external fun` | C++ Fonksiyon İmzası |
|---|---|
| `initEngine(surface: Surface)` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_initEngine(JNIEnv*, jobject, jobject)` |
| `destroyEngine()` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_destroyEngine(JNIEnv*, jobject)` |
| `setHardwareHz(hz: Int)` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_setHardwareHz(JNIEnv*, jobject, jint)` |
| `beginStroke(strokeId, toolTypeOrd, color, baseWidth)` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_beginStroke(JNIEnv*, jobject, jstring, jint, jint, jfloat)` |
| `addPoints(floatData, timestamps)` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_addPoints(JNIEnv*, jobject, jfloatArray, jlongArray)` |
| `endStroke()` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_endStroke(JNIEnv*, jobject)` |
| `updateGhostPoints(floatData, timestamps)` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_updateGhostPoints(JNIEnv*, jobject, jfloatArray, jlongArray)` |
| `clearGhostPoints()` | `Java_com_yourdomain_notes_engine_NativeDrawingEngine_clearGhostPoints(JNIEnv*, jobject)` |

---

## Bellek Güvenliği Kontrol Listesi

| Kaynak | Acquire | Release | Garanti |
|---|---|---|---|
| `ANativeWindow` | `ANativeWindow_fromSurface()` + `acquire()` | `~DrawingEngine()` → `release()` | RAII |
| `jfloatArray` pin | `GetFloatArrayElements()` | `ReleaseFloatArrayElements(JNI_ABORT)` | Her code path |
| `jlongArray` pin | `GetLongArrayElements()` | `ReleaseLongArrayElements(JNI_ABORT)` | Her code path (float null ise de) |
| `jstring` | `GetStringUTFChars()` | `ReleaseStringUTFChars()` | Her code path |
| `DrawingEngine` heap | `std::make_unique<>()` | `gEngine.reset()` | RAII / unique_ptr |

---

## Değişiklik Kuralları

- JNI fonksiyon imzasındaki paket/sınıf adı değiştirilirse C++ tarafındaki tüm `Java_com_yourdomain_notes_engine_NativeDrawingEngine_*` imzaları güncellenmelidir.
- `addPoints` dizi paketi düzeni (`[x, y, p, x, y, p, ...]`) değiştirilirse hem Kotlin hem C++ aynı anda güncellenmelidir.
- Ghost noktalar hiçbir zaman `activeStroke_.points`'e eklenmemelidir.
- `gEngine` global pointer'ına `initEngine` / `destroyEngine` dışından erişilmemelidir.