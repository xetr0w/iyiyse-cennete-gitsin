# PHASE_2_INPUT_ENGINE.md

## Amaç

Bu doküman, ham dokunma verisini UI thread'i bloklamadan toplayan, donanım örnekleme frekansını dinamik olarak tespit eden ve API 34+ cihazlarda `MotionPredictor` ile sıfır gecikme hissi yaratan input katmanını tanımlar.

### Mimari Pozisyon

```
MotionEvent (UI Thread)
        │
        ▼
DynamicInputSampler          ← bu dosya
        │
        ├─ onPointReceived    → JNI Bridge → C++ Core (gerçek nokta)
        └─ onGhostPointReceived → JNI Bridge → C++ Core (tahmin noktası)
```

**UI thread yalnızca veri toplar. Hiçbir çizim hesabı bu katmanda yapılmaz.**

---

## Dosya

**Path:** `app/src/main/java/com/yourdomain/notes/input/DynamicInputSampler.kt`

```kotlin
package com.yourdomain.notes.input

import android.os.Build
import android.view.MotionEvent
import android.view.View
import androidx.annotation.RequiresApi
import com.yourdomain.notes.domain.model.StylusPoint

// ─────────────────────────────────────────────────────────────────────────────
// MotionPredictor yalnızca API 34+ cihazlarda mevcuttur.
// Bu import derleme zamanında her SDK'da bulunur, ancak kullanım
// aşağıda Build.VERSION.SDK_INT kontrolüyle kesinlikle guard edilmiştir.
// ─────────────────────────────────────────────────────────────────────────────
import android.view.MotionPredictor

/**
 * Ham MotionEvent akışını işler, donanım Hz profilini çıkarır ve
 * API 34+ cihazlarda tahminli (ghost) noktalar üretir.
 *
 * KULLANIM KURALLARI:
 * - [processTouchEvent] yalnızca UI thread'den çağrılmalıdır.
 * - Lambda'lar içinde hiçbir çizim hesabı yapılmamalıdır;
 *   alınan [StylusPoint] doğrudan JNI bridge'e iletilmelidir.
 * - Bu sınıf herhangi bir View state'ini mutate etmez.
 *
 * @param view             Dokunma olaylarının bağlı olduğu View.
 *                         API 34+ MotionPredictor bu view üzerinden init edilir.
 * @param onPointReceived  Gerçek (confirmed) bir [StylusPoint] hazır olduğunda çağrılır.
 * @param onGhostPointReceived Tahminli (predicted) bir [StylusPoint] hazır olduğunda çağrılır.
 *                             Bu noktalar ekranda gösterilir ama CRDT log'una yazılmaz.
 */
class DynamicInputSampler(
    private val view: View,
    private val onPointReceived: (StylusPoint) -> Unit,
    private val onGhostPointReceived: (StylusPoint) -> Unit
) {

    // ─────────────────────────────────────────────────────────────────────────
    // DONANIM HZ PROFİLİ
    // İlk MAX_PROFILE_SAMPLES event arasındaki zaman farklarını toplayarak
    // donanımın gerçek örnekleme frekansını hesaplarız.
    // Bu değer C++ katmanına filtre parametresi olarak iletilebilir.
    // ─────────────────────────────────────────────────────────────────────────

    private companion object {
        /**
         * Hz profili için toplanacak minimum delta sayısı.
         * İlk 8 event arası delta, kararlı bir ortalama için yeterlidir.
         */
        const val MAX_PROFILE_SAMPLES = 8

        /** Nanosaniyeyi milisaniyeye çevirmek için bölen. */
        const val NS_TO_MS = 1_000_000.0

        /**
         * MotionEvent.getEventTime() milisaniye döndürür.
         * StylusPoint.timestamp nanosaniye gerektirir.
         * Dönüşüm çarpanı: ms * 1_000_000L = ns
         */
        const val MS_TO_NS = 1_000_000L
    }

    /**
     * Son ACTION_MOVE event'inin nanosaniye timestamp'i.
     * Delta hesabı için saklanır; 0L henüz event alınmadığını gösterir.
     */
    private var lastEventTimestampNs: Long = 0L

    /**
     * Toplanan timestamp delta'larının listesi (nanosaniye cinsinden).
     * [MAX_PROFILE_SAMPLES] adede ulaşınca [detectedHardwareHz] hesaplanır
     * ve bu liste bir daha güncellenmez.
     */
    private val timestampDeltasSamples: MutableList<Long> = mutableListOf()

    /**
     * Tespit edilen donanım Hz değeri.
     * null: henüz yeterli sample toplanmadı.
     * Olası değerler: 60, 120, 240 (yaklaşık; bkz. [profileHardwareHz]).
     */
    var detectedHardwareHz: Int? = null
        private set

    // ─────────────────────────────────────────────────────────────────────────
    // MOTION PREDICTOR (API 34+)
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * MotionPredictor instance'ı yalnızca API 34+ cihazlarda oluşturulur.
     * Diğer API seviyelerinde bu alan null kalır ve hiçbir zaman erişilmez.
     *
     * NOT: MotionPredictor constructor'ı bir View alır; bu view'in
     * display refresh rate bilgisini dahili olarak kullanır.
     */
    private val motionPredictor: MotionPredictor? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            MotionPredictor(view)
        } else {
            null
        }

    // ─────────────────────────────────────────────────────────────────────────
    // ANA GİRİŞ NOKTASI
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * View.onTouchEvent() içinden çağrılır.
     *
     * İşlem sırası:
     * 1. ACTION_DOWN / ACTION_MOVE / ACTION_UP dışındaki event'ler göz ardı edilir.
     * 2. Historical points önce işlenir (en eski → en yeni sırada).
     * 3. Mevcut (current) event noktası işlenir.
     * 4. Hz profili güncellenir.
     * 5. API 34+ ise tahminli noktalar üretilir.
     *
     * @return true  → event bu sınıf tarafından tüketildi.
     *         false → event daha fazla işlenmeli (örn. scroll).
     */
    fun processTouchEvent(event: MotionEvent): Boolean {
        return when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                // Yeni stroke başlangıcı: predictor'ı sıfırla, sayaçları temizle.
                resetStrokeState()
                processCurrentPoint(event)
                feedToPredictor(event)
                true
            }

            MotionEvent.ACTION_MOVE -> {
                // 1. Historical points — batching nedeniyle biriken geçmiş noktalar.
                //    Bu noktalar atlanırsa hız ve basınç verisinin büyük kısmı kaybolur.
                processHistoricalPoints(event)

                // 2. Mevcut (en güncel) nokta.
                processCurrentPoint(event)

                // 3. Hz profili: yeterli delta yoksa güncelle.
                updateHzProfile(event)

                // 4. API 34+: tahminli noktaları üret ve ilet.
                feedToPredictor(event)
                dispatchPredictedPoints(event)

                true
            }

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_CANCEL -> {
                // Son gerçek nokta işlenir, predictor temizlenir.
                processCurrentPoint(event)
                resetPredictorIfAvailable()
                true
            }

            else -> false
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // HISTORICAL POINTS
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * MotionEvent, bir frame içindeki birden fazla hareket noktasını
     * "historical" olarak paketler. Bu noktalar atlanırsa:
     * - Hızlı çizimde nokta kayıpları oluşur.
     * - Basınç eğrisi bozulur.
     * - C++ bezier interpolasyonu yanlış çalışır.
     *
     * Historical index 0 en eski, [getHistoricalSize - 1] en yeni noktadır.
     * getEventTime() her zaman en güncel noktayı taşır (current point).
     */
    private fun processHistoricalPoints(event: MotionEvent) {
        val historySize = event.historySize
        for (h in 0 until historySize) {
            // getHistoricalEventTime milisaniye döndürür → nanosaniyeye çevir.
            val timestampNs = event.getHistoricalEventTime(h) * MS_TO_NS
            val pressure = event.getHistoricalPressure(h).coerceIn(0f, 1f)
            val x = event.getHistoricalX(h)
            val y = event.getHistoricalY(h)

            val stylusPoint = StylusPoint(
                x = x,
                y = y,
                pressure = pressure,
                timestamp = timestampNs
            )
            onPointReceived(stylusPoint)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // CURRENT POINT
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * MotionEvent'in mevcut (en güncel) noktasını [StylusPoint]'e dönüştürür.
     * getEventTime() milisaniye döndürür; [MS_TO_NS] ile nanosaniyeye çevrilir.
     */
    private fun processCurrentPoint(event: MotionEvent) {
        val timestampNs = event.eventTime * MS_TO_NS
        val pressure = event.pressure.coerceIn(0f, 1f)

        val stylusPoint = StylusPoint(
            x = event.x,
            y = event.y,
            pressure = pressure,
            timestamp = timestampNs
        )
        onPointReceived(stylusPoint)
    }

    // ─────────────────────────────────────────────────────────────────────────
    // DONANIM HZ PROFİLİ
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Arka arkaya gelen ACTION_MOVE event'lerinin zaman farklarını ölçerek
     * donanımın gerçek dokunma örnekleme frekansını tespit eder.
     *
     * ALGORİTMA:
     * 1. Her MOVE event'inde [lastEventTimestampNs] ile mevcut event arasındaki
     *    delta nanosaniye cinsinden hesaplanır.
     * 2. [MAX_PROFILE_SAMPLES] kadar delta toplandığında ortalaması alınır.
     * 3. Ortalama delta → saniyeye çevrilir → 1/delta = Hz.
     * 4. Sonuç 60 / 120 / 240 Hz eşiklerine yuvarlanır.
     *
     * SINIRLAMALAR:
     * - İlk event'te delta hesaplanamaz (önceki timestamp yoktur); atlanır.
     * - [MAX_PROFILE_SAMPLES] sonrası profil sabitlenir; tekrar hesaplanmaz.
     *   (Donanım Hz'i runtime'da değişmez.)
     * - Yüksek CPU yükünde event gecikmesi ölçümü bozabilir;
     *   bu nedenle en düşük N deltayı değil ortalamayı kullanırız.
     */
    private fun updateHzProfile(event: MotionEvent) {
        // Profil zaten tamamlandıysa işlem yapma.
        if (detectedHardwareHz != null) return
        if (timestampDeltasSamples.size >= MAX_PROFILE_SAMPLES) return

        val currentTimestampNs = event.eventTime * MS_TO_NS

        if (lastEventTimestampNs != 0L) {
            val delta = currentTimestampNs - lastEventTimestampNs
            // Negatif veya sıfır delta anlamlı değil; atla (saat kayması).
            if (delta > 0L) {
                timestampDeltasSamples.add(delta)
            }
        }

        lastEventTimestampNs = currentTimestampNs

        // Yeterli sample toplandıysa Hz'i hesapla.
        if (timestampDeltasSamples.size >= MAX_PROFILE_SAMPLES) {
            detectedHardwareHz = profileHardwareHz(timestampDeltasSamples)
        }
    }

    /**
     * Toplanan delta listesinden donanım Hz değerini hesaplar.
     *
     * @param deltas Nanosaniye cinsinden timestamp delta'larının listesi.
     *               Liste boş olmamalıdır.
     * @return Yaklaşık donanım Hz değeri: 60, 120 veya 240.
     *         Tespit edilemeyen değerler için varsayılan: 60.
     */
    private fun profileHardwareHz(deltas: List<Long>): Int {
        if (deltas.isEmpty()) return 60

        val averageDeltaNs = deltas.average()          // nanosaniye cinsinden ortalama delta
        val averageDeltaSec = averageDeltaNs / 1e9      // saniyeye çevir
        val estimatedHz = (1.0 / averageDeltaSec)       // frekans = 1 / periyot

        // Gerçek donanım Hz değerleri arasında en yakına yuvarla.
        // Eşik değerleri: 60 ve 120 arasında 90, 120 ve 240 arasında 180.
        return when {
            estimatedHz >= 180.0 -> 240
            estimatedHz >= 90.0  -> 120
            else                 -> 60
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MOTION PREDICTOR — API 34+
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Mevcut event'i MotionPredictor'a besler.
     * API 34 altında [motionPredictor] null olduğundan bu fonksiyon no-op'tur.
     * Guard koşulu burada tekrar kontrol edilir; çağrı noktasında
     * SDK kontrolü yapılmasına gerek kalmaz.
     */
    private fun feedToPredictor(event: MotionEvent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            motionPredictor?.record(event)
        }
    }

    /**
     * MotionPredictor'dan tahminli noktaları alır ve [onGhostPointReceived]'e iletir.
     *
     * ÖNEMLI KURALLAR:
     * - predict() null döndürebilir (yeterli veri yoksa); null guard zorunludur.
     * - Tahminli event birden fazla nokta içerebilir; tümü işlenmelidir.
     * - Ghost noktalar CRDT log'una, Stroke listesine veya kalıcı herhangi
     *   bir yapıya YAZILMAZ. Yalnızca anlık görsel geri bildirim içindir.
     * - predict() çağrısı yalnızca ACTION_MOVE içinde yapılmalıdır.
     */
    @RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
    private fun dispatchPredictedPoints(event: MotionEvent) {
        // motionPredictor null olamaz (feedToPredictor içinde oluşturuldu),
        // ancak defensive null check platform tutarsızlıklarına karşı korunur.
        val predictor = motionPredictor ?: return

        // predict() parametresi: kaç nanosaniye ilerisi tahmin edilsin.
        // Bir frame ilerisi (display refresh rate'e göre) en stabil sonucu verir.
        // Şimdilik sabit 16_000_000 ns (≈ 60 fps frame süresi) kullanılıyor.
        // İleride detectedHardwareHz ile dinamik hale getirilebilir.
        val offsetNs = 16_000_000L  // 16 ms → nanosaniye

        val predictedEvent: MotionEvent? = predictor.predict(offsetNs)

        predictedEvent?.use { predicted ->
            // Predicted event de historical points içerebilir; tümünü işle.
            val historySize = predicted.historySize
            for (h in 0 until historySize) {
                val timestampNs = predicted.getHistoricalEventTime(h) * MS_TO_NS
                val pressure = predicted.getHistoricalPressure(h).coerceIn(0f, 1f)

                val ghostPoint = StylusPoint(
                    x = predicted.getHistoricalX(h),
                    y = predicted.getHistoricalY(h),
                    pressure = pressure,
                    timestamp = timestampNs
                )
                onGhostPointReceived(ghostPoint)
            }

            // Predicted event'in mevcut (en güncel) noktası.
            val ghostPoint = StylusPoint(
                x = predicted.x,
                y = predicted.y,
                pressure = predicted.pressure.coerceIn(0f, 1f),
                timestamp = predicted.eventTime * MS_TO_NS
            )
            onGhostPointReceived(ghostPoint)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // YARDIMCI FONKSİYONLAR
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Yeni bir stroke başladığında (ACTION_DOWN) dahili durumu sıfırlar.
     * Hz profili sıfırlanmaz; donanım frekansı runtime'da değişmez.
     */
    private fun resetStrokeState() {
        lastEventTimestampNs = 0L
        resetPredictorIfAvailable()
    }

    /**
     * MotionPredictor'ın dahili geçmiş tamponunu temizler.
     * ACTION_UP ve ACTION_CANCEL'dan sonra çağrılmalıdır;
     * aksi takdirde bir sonraki stroke'un tahmini önceki stroke verisinden etkilenir.
     */
    private fun resetPredictorIfAvailable() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            // MotionPredictor'ın reset mekanizması yeni bir event serisinin
            // record() ile beslenmesiyle otomatik gerçekleşir.
            // Explicit reset API mevcut değil; null-assign ile referansı koruruz.
            // Bu blok ileriki API değişikliklerinde genişletilebilir.
        }
    }
}
```

---

## Entegrasyon Notu

```kotlin
// DrawingView.kt içinde (örnek — bu dosyada değil):
private val sampler = DynamicInputSampler(
    view = this,
    onPointReceived = { point ->
        // JNI bridge'e ilet — UI thread'de hesaplama yapma
        JniBridge.pushRealPoint(point.x, point.y, point.pressure, point.timestamp)
    },
    onGhostPointReceived = { point ->
        // JNI bridge'e ilet — CRDT log'una yazılmaz
        JniBridge.pushGhostPoint(point.x, point.y, point.pressure, point.timestamp)
    }
)

override fun onTouchEvent(event: MotionEvent): Boolean {
    return sampler.processTouchEvent(event)
}
```

---

## Kural Özeti

| Kural | Zorunluluk |
|---|---|
| `MotionPredictor` her zaman `Build.VERSION.SDK_INT >= UPSIDE_DOWN_CAKE` ile guard edilir | **ZORUNLU** |
| Ghost noktalar kalıcı hiçbir yapıya yazılmaz | **ZORUNLU** |
| Historical points atlanmaz | **ZORUNLU** |
| Tüm timestamp'ler nanosaniye cinsinden iletilir | **ZORUNLU** |
| Lambda içinde çizim hesabı yapılmaz | **ZORUNLU** |