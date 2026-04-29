# PHASE_3_RENDER_CORE.md — Final Versiyon (Tüm Düzeltmeler Entegre)

---

## Mimari Felsefe ve Amaç

Bu doküman, Notia'nın "Sıfır Gecikme" vaadini görselleştiren GPU tabanlı çekirdek render motorunu tanımlar. Kotlin (Faz 2) katmanından gelen ham veriler [1], bu katmanda bir **Render Thread** tarafından devralınır, matematiksel olarak yumuşatılır ve Skia üzerinden doğrudan donanıma basılır.

Faz 3 mimarisi şu **6 temel sütuna** dayanır:

1. **Asenkron Render (Producer-Consumer):** Input thread (Kurye) asla bekletilmez. Veriyi bir kuyruğa bırakır ve işi Render Thread'e devreder. Bu, 120Hz/240Hz ekranlarda frame düşmesini (jank) engeller.

2. **GPU-Accelerated Smoothing:** Ham noktalar doğrudan çizilmez. Catmull-Rom Spline algoritmasıyla gerçek zamanlı olarak yumuşatılır. Buffer 4'ten az nokta içerdiğinde `lineTo` fallback devreye girer.

3. **Viewport Sync (Matrix Mapping):** Kotlin tarafındaki anlık Zoom/Pan değerleri ve fiziksel ekran boyutları C++ tarafındaki `SkMatrix` ile senkronize edilir. Canonical koordinatlar önce fiziksel ekrana oranlanır, ardından zoom/pan uygulanır. Çizim her zaman doğru koordinata düşer.

4. **Zarif Kaynak Yönetimi (RAII):** `ANativeWindow` ve Skia kaynakları C++ yaşam döngüsü kurallarına göre yönetilir. Skia nesneleri `sk_sp<T>` akıllı pointer'ı ile tutulur; bellek sızıntısı imkânsız hale getirilir.

5. **Ghost Point Isolation:** Tahminleme motorundan gelen noktalar [1] kalıcı veriye asla karışmaz; her frame'de swap ile güvenle okunup temizlenen geçici bir buffer'da yaşarlar.

6. **Tool Renderer Delegasyonu:** Çizim mantığı `DrawingEngine` içine gömülmez. Her araç tipi kendi `IToolRenderer` implementasyonunu çalıştırır [3]. `DrawingEngine` sadece orkestratördür.

---

## Mimari Pozisyon

```
DynamicInputSampler (Kotlin / Faz 2)
│   ham float[], long[] dizileri
▼
NativeDrawingEngine.kt          ← JNI Wrapper (Kotlin)
│   JNI sınırı
▼
drawing_engine_jni.cpp          ← JNI Köprüsü (C++)
│   RenderPoint struct'larına dönüşüm
▼
DrawingEngine (C++)             ← Producer-Consumer çekirdeği
│   std::mutex + std::deque + swap trick
▼
IToolRenderer (C++)             ← Araç stratejisi (Faz 4 bağlantısı)
│   BallpointRenderer / HighlighterRenderer vb.
▼
SkCanvas → EGL → ANativeWindow  ← GPU'ya commit + ekrana basma
```

---

## Dosya 1 — JNI Wrapper

**Path:** `app/src/main/java/com/yourdomain/notes/engine/NativeDrawingEngine.kt`

**Amacı:** C++ `DrawingEngine`'e tek erişim noktası. Faz 2'den gelen verileri [1] JNI sınırı üzerinden C++ Render Thread'ine besler. Bu sınıfın içinde **hiçbir hesaplama yapılmaz.**

**İçermesi gereken `external` fonksiyonlar:**

```
// Motor Yaşam Döngüsü
initEngine(surface: Surface)
destroyEngine()
setHardwareHz(hz: Int)

// Yüzey Boyutu Bildirimi
// SurfaceView boyutu değiştiğinde (rotation, multi-window, foldable)
// Kotlin tarafı bu fonksiyonu çağırır. C++ ANativeWindow'dan tahmin etmez.
setSurfaceSize(widthPx: Int, heightPx: Int)

// Viewport Senkronizasyonu
updateViewport(scale: Float, offsetX: Float, offsetY: Float)

// Stroke Akışı (Producer)
beginStroke(strokeId: String, toolTypeOrd: Int, color: Int, baseWidth: Float)
addPoints(floatData: FloatArray, timestamps: LongArray)
endStroke()

// Ghost Noktalar
updateGhostPoints(floatData: FloatArray, timestamps: LongArray)
clearGhostPoints()
```

**`setSurfaceSize` Ne Zaman Çağrılır:**
- `SurfaceHolder.Callback.surfaceCreated` — Surface ilk oluştuğunda
- `SurfaceHolder.Callback.surfaceChanged` — Rotation, multi-window veya foldable geçişlerinde
- `initEngine`'den hemen sonra

**Kritik Not:** `floatData` dizisi `[x0, y0, p0, x1, y1, p1, ...]` formatında paketlenmiş gelir. `timestamps` dizisi nanosaniye cinsindendir [2]. JNI tarafı bu formatı `RenderPoint` struct dizisine dönüştürür [4].

---

## Dosya 2 — JNI Köprüsü

**Path:** `app/src/main/cpp/drawing_engine_jni.cpp`

**Amacı:** Kotlin'den gelen JNI çağrılarını karşılar, `jfloatArray` / `jlongArray` dizilerini `std::vector<RenderPoint>`'e çevirir [4] ve `DrawingEngine`'in ilgili metodunu çağırır. Kendisi **hiçbir hesaplama yapmaz.**

**Uygulanacak Kritik Kurallar:**

- `GetFloatArrayElements` çağrıldıktan sonra **her kod yolunda** (hata durumları dahil) `ReleaseFloatArrayElements` çağrılmalıdır. Flag olarak `JNI_ABORT` kullanılır; Kotlin heap'ine gereksiz write-back yapılmaz, GC baskısı oluşmaz [4].
- `floatData` uzunluğunun 3'ün katı olup olmadığı kontrol edilir. Değilse `LOGE` ile loglanır ve erken dönülür [4].
- `DrawingEngine` instance'ı `static std::unique_ptr<DrawingEngine>` olarak tutulur. `initEngine`'de oluşturulur, `destroyEngine`'de `reset()` ile yok edilir.

**Gelecek Optimizasyon Notu — `GetPrimitiveArrayCritical`:**
`GetFloatArrayElements` JVM keyfine göre veriyi kopyalayabilir. Profiler'da gerçek bir GC pause sorunu görülürse `GetPrimitiveArrayCritical` ile zero-copy'ye geçilebilir; bu yöntem JVM'e doğrudan bellek adresi vererek GC'yi dondurur. Ancak critical bölge içinde başka JNI çağrısı, Java kodu veya mutex lock yapılamaz. Mevcut kodda `LOGE` çağrıları critical bölge dışına taşınmadan bu geçiş yapılamaz. Bu optimizasyon MVP sonrasına ertelenir.

**RenderPoint Struct:**

```cpp
struct RenderPoint {
    float   x, y, pressure;
    int64_t timestamp;   // Nanosaniye — milisaniyeye ÇEVRİLMEZ [2]
};
```

---

## Dosya 3 — DrawingEngine Header

**Path:** `app/src/main/cpp/DrawingEngine.h`

**Amacı:** Producer-Consumer mimarisinin çekirdeği. JNI çağrıları (Producer) veriyi kuyruğa koyar; Render Thread (Consumer) kuyruktan swap ile çekerek Skia'ya verir.

**Sınıf İskeleti ve Açıklamaları:**

```cpp
class DrawingEngine {
public:
    explicit DrawingEngine(ANativeWindow* window);
    ~DrawingEngine();
    // RAII: destructor sırası:
    //   1. running_ = false
    //   2. cv_.notify_all()
    //   3. renderThread_.join()
    //   4. destroySkia()   ← EGL kaynakları burada serbest bırakılır
    //   5. ANativeWindow_release(window_)

    // ── Producer Arayüzü (JNI thread'inden çağrılır) ──────────────────
    void setHardwareHz(int hz);
    void setSurfaceSize(int widthPx, int heightPx);
    void updateViewport(float scale, float offsetX, float offsetY);
    void beginStroke(const std::string& id, int toolOrd, int color, float width);
    void addPoints(const std::vector<RenderPoint>& points);
    void endStroke();
    void updateGhostPoints(const std::vector<RenderPoint>& points);
    void clearGhostPoints();

private:
    // ── Render Thread (Consumer) ───────────────────────────────────────
    std::thread             renderThread_;
    std::mutex              queueMutex_;
    std::condition_variable cv_;
    std::atomic<bool>       running_{false};
    void renderLoop();

    // ── Veri Yapıları ──────────────────────────────────────────────────
    std::deque<RenderPoint>  incomingQueue_;  // Gerçek noktalar (mutex korumalı)
    std::vector<RenderPoint> ghostBuffer_;    // Hayalet noktalar (mutex korumalı)
    std::vector<RenderPoint> splineBuffer_;   // Son 4 nokta — Catmull-Rom sliding window

    // ── Aktif Stroke State ─────────────────────────────────────────────
    struct ActiveStrokeState {
        std::string strokeId;
        int         toolOrd   = 0;
        int         color     = 0;
        float       baseWidth = 1.0f;
        bool        isActive  = false;
    } activeStroke_;

    // endStroke sinyali için atomic flag.
    // renderLoop her iterasyonda kontrol eder;
    // true görürse activeRenderer_->endStroke() tetikler ve false yapar.
    std::atomic<bool> strokeEndPending_{false};

    // ── Viewport ve Yüzey Boyutu (Atomic) ─────────────────────────────
    std::atomic<float> vScale_{1.0f};
    std::atomic<float> vOffsetX_{0.0f};
    std::atomic<float> vOffsetY_{0.0f};
    std::atomic<int>   surfaceWidthPx_{1};    // 0'a bölme koruması için 1 başlar
    std::atomic<int>   surfaceHeightPx_{1};

    // ── EGL ve Skia Kaynakları ─────────────────────────────────────────
    // EGL kurulum zinciri: ANativeWindow → EGLSurface → EGLContext → GrDirectContext
    // std::unique_ptr KULLANILMAZ. Skia nesneleri sk_sp<T> ile yönetilir.
    // std::unique_ptr<GrDirectContext> → derleme hatası verir.
    ANativeWindow*         window_       = nullptr;
    int                    hardwareHz_   = 60;
    EGLDisplay             eglDisplay_   = EGL_NO_DISPLAY;
    EGLSurface             eglSurface_   = EGL_NO_SURFACE;
    EGLContext             eglContext_   = EGL_NO_CONTEXT;
    sk_sp<GrDirectContext> grContext_;
    sk_sp<SkSurface>       skSurface_;

    // ── Tool Renderer (Faz 4 Bağlantısı) ──────────────────────────────
    // Somut renderer sınıfları import edilmez.
    // DrawingEngine sadece IToolRenderer* arayüzüne bağımlıdır.
    // Factory: activeRenderer_ = ToolRendererFactory::create(toolOrd, params)
    // ToolRendererFactory Faz 4'te tanımlanır [3].
    std::unique_ptr<IToolRenderer> activeRenderer_;

    // ── Özel Render Metodları ──────────────────────────────────────────
    void initSkia();
    void destroySkia();
    void applyViewport(SkCanvas* canvas);
    void processIncomingPoints(SkCanvas*                        canvas,
                               const std::vector<RenderPoint>& points);
    void drawSmoothSegment(SkCanvas*          canvas,
                           const RenderPoint& p0, const RenderPoint& p1,
                           const RenderPoint& p2, const RenderPoint& p3);
    void renderGhostLayer(SkCanvas*                        canvas,
                          const std::vector<RenderPoint>& localGhost);
};
```

---

## Dosya 4 — DrawingEngine Implementation

**Path:** `app/src/main/cpp/DrawingEngine.cpp`

### 4.1 — Constructor ve Destructor

Constructor `initSkia()`'yı çağırır ve `renderThread_`'i başlatır. Destructor sırasıyla şunları yapar:

1. `running_ = false` set eder
2. `cv_.notify_all()` ile Render Thread'i uyandırır
3. `renderThread_.join()` ile thread'in temiz kapanmasını bekler
4. `destroySkia()` çağrılır — EGL kaynakları şu sırayla serbest bırakılır:
   - `skSurface_` reset
   - `grContext_` flush + reset
   - `eglDestroySurface(eglDisplay_, eglSurface_)`
   - `eglDestroyContext(eglDisplay_, eglContext_)`
   - `eglTerminate(eglDisplay_)`
5. `ANativeWindow_release(window_)` ile Surface serbest bırakılır

### 4.2 — initSkia ve EGL Kurulum Stratejisi

```
initSkia() kurulum zinciri (sıra kritiktir):

1. eglGetDisplay(EGL_DEFAULT_DISPLAY)  → eglDisplay_
2. eglInitialize(eglDisplay_, ...)
3. eglChooseConfig(...)                → EGLConfig seç
4. eglCreateWindowSurface(eglDisplay_, config, window_, ...) → eglSurface_
5. eglCreateContext(eglDisplay_, config, EGL_NO_CONTEXT, ...) → eglContext_
6. eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)
7. GrGLInterface::MakeNativeGL()       → Skia'ya GL backend ver
8. GrDirectContext::MakeGL(interface)  → grContext_
9. SkSurface::MakeFromBackendRenderTarget(...) → skSurface_

Neden bu yol?
  ANativeWindow → EGL → GrDirectContext zinciri,
  Skia'nın Android GPU backend'ini doğrudan kullanmasını sağlar.
  Bu yolda flushAndSubmit() GPU backbuffer'a yazar;
  eglSwapBuffers() ise backbuffer'ı ekrana (frontbuffer) taşır.
  İkisi birlikte zorunludur — biri olmadan ekran siyah kalır.
```

### 4.3 — Producer Metodları

**`addPoints`:** `queueMutex_` kilidi altında `incomingQueue_`'ya ekler, kilidi bırakır, `cv_.notify_one()` çağırır. İçinde **Skia kodu çalışmaz.** `activeStroke_.isActive == false` ise işlem yapılmaz — orphan nokta oluşturulamaz.

**`updateGhostPoints`:** `queueMutex_` kilidi altında `ghostBuffer_`'ı tamamen **replace** eder. Her çağrı en güncel ghost setini temsil eder [1].

**`clearGhostPoints`:** `queueMutex_` kilidi altında `ghostBuffer_.clear()` çağrılır. Ardından **mutlaka** `cv_.notify_one()` çağrılır. Bu çağrı olmadan render thread uyumaya devam eder ve ghost noktalar bir sonraki gerçek çizim gelene kadar ekranda takılı kalır.

**`updateViewport`:** Üç `atomic<float>`'a store eder. Mutex gerekmez.

**`setSurfaceSize`:** `surfaceWidthPx_` ve `surfaceHeightPx_` atomic değerlerini günceller. C++ tarafı `ANativeWindow_getWidth/Height`'a bel bağlamaz [4].

**`beginStroke`:** `activeStroke_` struct'ını doldurur, `isActive = true` yapar. `splineBuffer_`'ı temizler. `strokeEndPending_ = false` yapar. `ToolRendererFactory::create(toolOrd, params)` ile `activeRenderer_`'ı set eder [3].

**`endStroke`:** `strokeEndPending_ = true` set eder, `cv_.notify_one()` çağırır. `activeStroke_.isActive = false` yapar.

### 4.4 — Render Loop (Consumer)

```
renderLoop() akış mantığı:

1. condition_variable ile bekle:
   (incomingQueue_ dolu) VEYA
   (ghostBuffer_ dolu) VEYA
   (strokeEndPending_ == true) VEYA
   (running_ == false)

2. running_ == false ise döngüden çık (temiz kapanma).

3. SWAP TRICK — O(1) performans, mutex altında tek yapılan iş:
   ┌─────────────────────────────────────────────────────────┐
   │  std::deque<RenderPoint>  localQueue;                   │
   │  std::vector<RenderPoint> localGhost;                   │
   │  {                                                      │
   │    std::unique_lock<std::mutex> lock(queueMutex_);      │
   │    localQueue.swap(incomingQueue_);                     │
   │    localGhost.swap(ghostBuffer_);                       │
   │  } // ← Lock burada düşer. Producer anında devam eder.  │
   └─────────────────────────────────────────────────────────┘
   Tek tek eleman kopyalamak YASAKTIR.
   Kuyrukta 10.000 nokta olsa bile swap O(1)'dir.

4. SkCanvas al: skSurface_->getCanvas()

5. applyViewport(canvas) çağır.

6. processIncomingPoints(canvas, localQueue) çağır.
   → Catmull-Rom smoothing burada çalışır [3].
   → IToolRenderer->addPoints() delegate edilir.

7. strokeEndPending_ == true ise:
   → activeRenderer_->endStroke(canvas) çağır [3].
   → strokeEndPending_ = false yap.
   → splineBuffer_'ı temizle.

8. renderGhostLayer(canvas, localGhost) çağır.
   → localGhost ile çizim yapılır.
   → Bu layer kalıcı path'e eklenmez [1].
   → localGhost boşsa bu adım atlanır; ghost noktalar
     temizlenmiş demektir (clearGhostPoints senaryosu).

9. GPU Commit — İKİ ADIM, İKİSİ DE ZORUNLU:
   ┌─────────────────────────────────────────────────────────┐
   │  grContext_->flushAndSubmit();                          │
   │  // Skia hesaplamalarını GPU backbuffer'a gönderir.     │
   │                                                         │
   │  eglSwapBuffers(eglDisplay_, eglSurface_);              │
   │  // Backbuffer'ı frontbuffer ile takas eder.            │
   │  // V-Sync burada yakalanır.                            │
   │  // Bu çağrı olmadan ekran siyah kalır.                 │
   └─────────────────────────────────────────────────────────┘
   Her render loop iterasyonunda bir kez çağrılır.
   "Her nokta batch'inde flush" yapılmaz; GPU gereksiz yorulmaz.
```

**Ghost Buffer Davranışı Notu:** Swap sonrası `ghostBuffer_` boşalır. Faz 2'deki `MotionPredictor` [1] sürekli yeni ghost basacağı için pratikte her frame'de `updateGhostPoints` çağrılır ve buffer yeniden dolar. `clearGhostPoints` çağrıldığında ise `cv_.notify_one()` render thread'i uyandırır, `localGhost` boş gelir, ghost layer atlanır ve `eglSwapBuffers` ile temiz frame ekrana basılır.

### 4.5 — Catmull-Rom Smoothing

**Neden Catmull-Rom?** Kontrol noktalarının doğrudan **içinden geçer.** Bezier ve B-Spline yaklaştırma (approximation) yapar — noktaların dışından dolaşır ve "kalem ucunun gerisinde kalıyor" hissini yaratır. Catmull-Rom interpolasyon yaptığı için sıfır gecikme vaadini destekler [1].

**Formül:**
```
P(t) = 0.5 * [
    (2·P1)
  + (-P0 + P2)·t
  + (2·P0 - 5·P1 + 4·P2 - P3)·t²
  + (-P0 + 3·P1 - 3·P2 + P3)·t³
]
t ∈ [0, 1]
```

**`processIncomingPoints` davranışı:**

```
Her yeni nokta splineBuffer_'a eklenir.

splineBuffer_.size() < 4  → Fallback: IToolRenderer'a doğrudan lineTo ver.
                             Bu edge case stroke'un ilk 1-3 noktasında yaşanır.
                             Görsel fark ihmal edilebilir düzeydedir.

splineBuffer_.size() >= 4 → Son 4 nokta ile Catmull-Rom segment hesapla.
                             t adımı hardwareHz_'e göre dinamik belirlenir:
                               120Hz → daha az adım (ekran zaten smooth)
                               60Hz  → daha çok adım (ara noktaları doldur)
                             Hesaplanan segment IToolRenderer->addPoints()'e
                             verilir [3].

splineBuffer_ sliding window gibi çalışır:
Her yeni nokta eklenince en eski nokta atılır (max 4 eleman).
Her endStroke'ta tamamen temizlenir.
```

**`drawSmoothSegment` parametreleri:** `(p0, p1, p2, p3)` — p1 ile p2 arasındaki segment çizilir; p0 ve p3 eğrinin giriş/çıkış yönünü belirler [3].

### 4.6 — applyViewport

```
Amaç: 2000x3000 canonical alanı [2] fiziksel ekrana doğru maplemek.

İki dönüşüm katmanı vardır ve sırası kritiktir:

KATMAN 1 — Canonical → Physical (screenRatio):
  Faz 2 [1] ham fiziksel pikseli 2000x3000 canonical'a dönüştürdü.
  Bu dönüşümün tersini uygulamak gerekir.

  canonicalWidth  = 2000.0f   (Faz 1 sabiti [2])
  canonicalHeight = 3000.0f   (Faz 1 sabiti [2])

  surfaceW = (float)surfaceWidthPx_.load()
  surfaceH = (float)surfaceHeightPx_.load()

  screenRatioX = surfaceW / canonicalWidth
  screenRatioY = surfaceH / canonicalHeight

  NOT: ANativeWindow_getWidth/Height() KULLANILMAZ.
  Boyut Kotlin tarafından setSurfaceSize() ile iletilir.
  Rotation/multi-window/foldable geçişlerinde Kotlin günceller.

KATMAN 2 — Zoom / Pan (scale, offsetX, offsetY):
  Kullanıcının anlık zoom ve pan durumu uygulanır.

SkMatrix kurulumu (sıra önemlidir):

  matrix.setScale(screenRatioX, screenRatioY)     // Katman 1
  matrix.postScale(scale, scale)                   // Katman 2 — zoom
  matrix.postTranslate(-offsetX * scale,           // Katman 2 — pan
                       -offsetY * scale)
  canvas->setMatrix(matrix)

Neden iki ayrı katman?
  Canonical dönüşüm (Katman 1) ekran boyutuna bağlıdır; nadiren değişir.
  Zoom/Pan (Katman 2) kullanıcı etkileşimine bağlıdır; sürekli değişir.
  İkisini tek matrix'te karıştırmak debug'ı imkânsızlaştırır.
```

---

## ⛔ Yapay Zeka ve Geliştiriciler İçin Kesin Kurallar

1. **Thread Bloklama YASAĞI:** `addPoints`, `beginStroke` gibi JNI fonksiyonları içinde Skia çağrısı veya ağır hesaplama yapılamaz. Veri kuyruğa atılır, dönülür.

2. **JNI Memory Leak YASAĞI:** `GetFloatArrayElements` çağrıldıysa, hata durumları dahil her kod yolunda `ReleaseFloatArrayElements` çağrılmalıdır. Flag: `JNI_ABORT` [4].

3. **Smoothing YASAĞI:** Catmull-Rom veya herhangi bir yumuşatma algoritması Kotlin tarafında çalıştırılamaz. Kotlin sadece ham veriyi taşır [1].

4. **Ghost Point Kirletme YASAĞI:** Ghost noktalar `incomingQueue_`'ya veya `Page` modeline eklenemez. Sadece `ghostBuffer_`'da yaşarlar [2].

5. **Viewport Ham Piksel YASAĞI:** Çizim koordinatları ham piksellerle işlenemez. Her zaman `applyViewport` üzerinden geçen `SkMatrix` ile render edilir.

6. **Zaman Birimi YASAĞI:** Zaman damgaları C++ tarafında da `int64_t` nanosaniye olarak korunur. Milisaniyeye çevrilmez [2].

7. **Flush Frekansı YASAĞI:** `flushAndSubmit` her nokta batch'inde çağrılamaz. Her render loop iterasyonunda en fazla bir kez çağrılır.

8. **Orphan Nokta YASAĞI:** `addPoints` ve `endStroke`, `activeStroke_.isActive == true` olmadan işlem yapamaz.

9. **O(N) Mutex YASAĞI:** `queueMutex_` altında döngü ile tek tek kopyalama yapılamaz. Kuyruk transferi yalnızca `swap()` ile yapılır.

10. **Boyut Tahmini YASAĞI:** C++ tarafı `ANativeWindow_getWidth/Height` ile yüzey boyutunu kendi başına okuyamaz. Boyut her zaman Kotlin tarafından `setSurfaceSize()` ile iletilir.

11. **`sk_sp` Yerine `unique_ptr` YASAĞI:** Skia nesneleri (`GrDirectContext`, `SkSurface`) `std::unique_ptr` ile tutulamaz. Skia'nın kendi referans sayaç sistemi olan `sk_sp<T>` kullanılır.

12. **Yarım GPU Commit YASAĞI:** `flushAndSubmit()` tek başına yeterli değildir. Her render iterasyonunda `eglSwapBuffers(eglDisplay_, eglSurface_)` de çağrılmalıdır. Biri eksik olursa ekran siyah kalır veya frame'ler ekrana yansımaz.

13. **`clearGhostPoints` Sessiz Temizleme YASAĞI:** `clearGhostPoints()` yalnızca `ghostBuffer_`'ı temizleyemez. Her zaman ardından `cv_.notify_one()` çağrılmalıdır; aksi hâlde ghost noktalar render thread'i uyandırana kadar ekranda takılı kalır.

---

## Faz 4 Bağlantısı

Bu doküman `IToolRenderer` arayüzünü **kullanır** ama tanımlamaz. `IToolRenderer`'ın tam tanımı, `BallpointRenderer`, `HighlighterRenderer` implementasyonları [3] ve Command Pattern ile Undo/Redo mimarisi **PHASE_4_TOOL_STRATEGY.md** dosyasında yer alır.

`DrawingEngine` bu arayüze yalnızca pointer üzerinden bağımlıdır. Somut renderer sınıflarını import etmez. `ToolRendererFactory` Faz 4'te tanımlanır; `DrawingEngine` yalnızca `IToolRenderer*` tipini bilir. Bu, Faz 3 ile Faz 4 arasındaki bağımlılığı tek yönlü ve gevşek tutar.
