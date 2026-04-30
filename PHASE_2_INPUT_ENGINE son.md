# PHASE_2_INPUT_ENGINE.md

## Mimari Felsefe ve Amaç

Bu doküman, Android'in UI thread'ini yalnızca "veri toplamak" için kullanan, hiçbir ağır hesaplama yapmadan ham veriyi donanımdan çekip canonical (sanal kağıt) formata çevirerek JNI köprüsüne fırlatan giriş katmanını tanımlar.

Tüm Faz 2 mimarisi şu 3 temel sütuna dayanır:
1.  **Kurye Metodu (No-Filter):** UI thread yalnızca veri toplar. Yumuşatma (Smoothing), filtreleme (Kalman) veya mikro-segmentasyon işlemleri KESİNLİKLE BURADA YAPILMAZ; C++ katmanına bırakılır.
2.  **Sıfır Gecikme (Zero-Latency):** İşletim sisteminin V-Sync bekleme odası iptal edilir (`Unbuffered Dispatch`) ve 120Hz/240Hz ekranlara özel dinamik donanım tahminlemesi (MotionPredictor) yapılır.
3.  **Zoom/Pan Dayanıklılığı:** Ham pikseller modele yazılmaz. Tüm dokunuşlar, sayfanın anlık Zoom ve Kaydırma (Pan) değerlerine göre Faz 1'de belirlenen 2000x3000'lik sanal kağıda oranlanır.

> **Yapay Zeka Asistanları İçin Not:** Bu dosya uygulamanın giriş katmanı anayasasıdır. Kod bloklarındaki korumalar (API Guard'ları, NaN engellemeleri, Coroutine Cleanup'ları) Android'in ve donanımların uç durumlarına (edge cases) karşı yazılmıştır ve değiştirilemez.

---

## Modeller ve Çekirdek Motor (Path: `app/src/main/java/com/yourdomain/notes/input/DynamicInputSampler.kt`)

```kotlin
package com.yourdomain.notes.input

import android.os.Build
import android.view.MotionEvent
import android.view.View
import android.view.MotionPredictor
import androidx.annotation.RequiresApi
import com.yourdomain.notes.domain.model.StylusPoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

// ─────────────────────────────────────────────────────────────────────────────
// 1. YAPILANDIRMA VE VIEWPORT (ZOOM/PAN MİMARİSİ)
// Ayarların kod içine gömülmemesi ve ViewportTransform ile UI'dan bağımsız 
// koordinat hesabı yapabilmek için kullanılır.
// ─────────────────────────────────────────────────────────────────────────────

data class InputSamplerConfig(
    val doubleClickThresholdMs: Long = 250L,
    val canonicalWidthPx: Float = 2000f,  // Faz 1 Standardı
    val canonicalHeightPx: Float = 3000f  // Faz 1 Standardı
)

data class ViewportTransform(
    val zoomScale: Float = 1f,
    val scrollOffsetX: Float = 0f,
    val scrollOffsetY: Float = 0f
)

enum class StylusButtonAction {
    HOLD_ERASER,
    SINGLE_CLICK,
    DOUBLE_CLICK
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. DYNAMIC INPUT SAMPLER (GİRİŞ MOTORU)
// Tüm MotionEvent ve HoverEvent'leri yakalayan ana sınıf.
// ─────────────────────────────────────────────────────────────────────────────

class DynamicInputSampler(
    private val view: View,
    private val coroutineScope: CoroutineScope,
    private val config: InputSamplerConfig = InputSamplerConfig(),
    
    // Neden @Volatile? ViewModel bu değeri farklı bir iş parçacığından güncelleyebilir.
    // Thread safety (izlek güvenliği) için her zaman RAM'deki en güncel değer okunmalıdır.
    @Volatile var viewportTransform: ViewportTransform = ViewportTransform(),
    
    private val onPointReceived: (StylusPoint) -> Unit,
    private val onGhostPointReceived: (StylusPoint) -> Unit,
    private val onHoverReceived: (canonicalX: Float, canonicalY: Float, tilt: Float, orientation: Float) -> Unit,
    private val onButtonAction: (StylusButtonAction, isHolding: Boolean) -> Unit
) {

    private companion object {
        const val MAX_PROFILE_SAMPLES = 8
        const val MS_TO_NS = 1_000_000L
        const val INVALID_POINTER_ID = -1
    }

    private var lastEventTimestampNs: Long = 0L
    private val timestampDeltasSamples: MutableList<Long> = mutableListOf()
    var detectedHardwareHz: Int? = null
        private set

    // S-Pen Tuş Yöneticisi için State değişkenleri
    private var lastButtonDownTimeMs: Long = 0L
    private var isButtonCurrentlyPressed: Boolean = false
    private var pendingSingleClickJob: Job? = null

    // Neden activeStylusPointerId? Android'de dokunma indeksi (index) değişebilir.
    // Kalemin kimliğini (ID) takip etmek, çoklu dokunmada (el+kalem) verinin kaymasını önler.
    private var activeStylusPointerId: Int = INVALID_POINTER_ID

    // API 34+ Tahminleme Motoru
    private val motionPredictor: MotionPredictor? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            MotionPredictor(view)
        } else {
            null
        }

    // ─────────────────────────────────────────────────────────────────────────
    // YARDIMCI FONKSİYON: CANONICAL DÖNÜŞÜM (HAM PİKSEL YASAĞI)
    // Neden w == 0 kontrolü var? Layout henüz tamamlanmadan gelen dokunuşlarda
    // 0'a bölme hatasını (NaN) ve veritabanı yozlaşmasını engellemek için.
    // ─────────────────────────────────────────────────────────────────────────
    private fun toCanonicalX(rawX: Float): Float {
        val w = view.width
        if (w == 0) return 0f 
        return (rawX / w) * (config.canonicalWidthPx / viewportTransform.zoomScale) + 
                viewportTransform.scrollOffsetX
    }

    private fun toCanonicalY(rawY: Float): Float {
        val h = view.height
        if (h == 0) return 0f 
        return (rawY / h) * (config.canonicalHeightPx / viewportTransform.zoomScale) + 
                viewportTransform.scrollOffsetY
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANA GİRİŞ NOKTASI 1: DOKUNMA OLAYLARI (TOUCH EVENTS)
    // ─────────────────────────────────────────────────────────────────────────
    fun processTouchEvent(event: MotionEvent): Boolean {
        // Neden API 26 Guard? requestUnbufferedDispatch(event) metodu API 21-25 
        // arasında crash riski taşıdığı için güvenli liman Oreo (API 26) seçilmiştir.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            view.requestUnbufferedDispatch(event)
        }

        val action = event.actionMasked

        // 1. Yeni Dokunuş: Kalem ID'sini tespit et ve kilitle
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            for (i in 0 until event.pointerCount) {
                if (event.getToolType(i) == MotionEvent.TOOL_TYPE_STYLUS) {
                    activeStylusPointerId = event.getPointerId(i)
                    break
                }
            }
        }

        if (activeStylusPointerId == INVALID_POINTER_ID) return false

        // 2. Kalemin anlık indeksini bul (İndeks değişse de ID değişmez)
        val stylusIndex = event.findPointerIndex(activeStylusPointerId)
        if (stylusIndex == -1) {
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                activeStylusPointerId = INVALID_POINTER_ID
            }
            return false
        }

        processButtonState(event)

        return when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                resetStrokeState()
                processCurrentPoint(event, stylusIndex)
                feedToPredictor(event)
                true
            }
            MotionEvent.ACTION_MOVE -> {
                processHistoricalPoints(event, stylusIndex)
                processCurrentPoint(event, stylusIndex)
                updateHzProfile(event)
                feedToPredictor(event)
                
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                    dispatchPredictedPoints(event, stylusIndex)
                }
                true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                processCurrentPoint(event, stylusIndex)
                resetPredictorIfAvailable()
                activeStylusPointerId = INVALID_POINTER_ID
                true
            }
            else -> false
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANA GİRİŞ NOKTASI 2: HAVADA GEZİNME (HOVER EVENTS)
    // ─────────────────────────────────────────────────────────────────────────
    fun processHoverEvent(event: MotionEvent): Boolean {
        var stylusIndex = -1
        for (i in 0 until event.pointerCount) {
            if (event.getToolType(i) == MotionEvent.TOOL_TYPE_STYLUS) {
                stylusIndex = i
                break
            }
        }

        if (stylusIndex == -1) return false

        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_HOVER_ENTER -> {
                val cX = toCanonicalX(event.getX(stylusIndex))
                val cY = toCanonicalY(event.getY(stylusIndex))
                val tilt = event.getAxisValue(MotionEvent.AXIS_TILT, stylusIndex)
                val orient = event.getAxisValue(MotionEvent.AXIS_ORIENTATION, stylusIndex)
                onHoverReceived(cX, cY, tilt, orient)
                return true
            }
            MotionEvent.ACTION_HOVER_EXIT -> return true
        }
        return false
    }

    // ─────────────────────────────────────────────────────────────────────────
    // S-PEN TUŞU YÖNETİCİSİ (STATE MACHINE)
    // ─────────────────────────────────────────────────────────────────────────
    private fun processButtonState(event: MotionEvent) {
        val isPressed = (event.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY) != 0
        val currentTimeMs = System.currentTimeMillis()

        if (isPressed && !isButtonCurrentlyPressed) {
            isButtonCurrentlyPressed = true
            val timeSinceLastDown = currentTimeMs - lastButtonDownTimeMs

            if (timeSinceLastDown < config.doubleClickThresholdMs) {
                pendingSingleClickJob?.cancel() 
                onButtonAction(StylusButtonAction.DOUBLE_CLICK, false)
                lastButtonDownTimeMs = 0L 
            } else {
                onButtonAction(StylusButtonAction.HOLD_ERASER, true)
                lastButtonDownTimeMs = currentTimeMs
            }
        } else if (!isPressed && isButtonCurrentlyPressed) {
            isButtonCurrentlyPressed = false
            val holdDuration = currentTimeMs - lastButtonDownTimeMs
            
            if (holdDuration < config.doubleClickThresholdMs) {
                pendingSingleClickJob = coroutineScope.launch {
                    delay(config.doubleClickThresholdMs)
                    onButtonAction(StylusButtonAction.SINGLE_CLICK, false)
                }
            }
            onButtonAction(StylusButtonAction.HOLD_ERASER, false)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // NOKTA İŞLEME VE TAHMİNLEME (GHOST POINTS)
    // ─────────────────────────────────────────────────────────────────────────
    private fun processHistoricalPoints(event: MotionEvent, stylusIndex: Int) {
        val historySize = event.historySize
        for (h in 0 until historySize) {
            val stylusPoint = StylusPoint(
                x = toCanonicalX(event.getHistoricalX(stylusIndex, h)),
                y = toCanonicalY(event.getHistoricalY(stylusIndex, h)),
                pressure = event.getHistoricalPressure(stylusIndex, h).coerceIn(0f, 1f),
                tilt = event.getHistoricalAxisValue(MotionEvent.AXIS_TILT, stylusIndex, h),
                orientation = event.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, stylusIndex, h),
                timestamp = event.getHistoricalEventTime(h) * MS_TO_NS
            )
            onPointReceived(stylusPoint)
        }
    }

    private fun processCurrentPoint(event: MotionEvent, stylusIndex: Int) {
        val stylusPoint = StylusPoint(
            x = toCanonicalX(event.getX(stylusIndex)),
            y = toCanonicalY(event.getY(stylusIndex)),
            pressure = event.getPressure(stylusIndex).coerceIn(0f, 1f),
            tilt = event.getAxisValue(MotionEvent.AXIS_TILT, stylusIndex),
            orientation = event.getAxisValue(MotionEvent.AXIS_ORIENTATION, stylusIndex),
            timestamp = event.eventTime * MS_TO_NS
        )
        onPointReceived(stylusPoint)
    }

    @RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
    private fun dispatchPredictedPoints(event: MotionEvent, stylusIndex: Int) {
        val predictor = motionPredictor ?: return

        val offsetNs = when (detectedHardwareHz) {
            240  -> 4_166_666L  // ~4.2ms
            120  -> 8_333_333L  // ~8.3ms
            else -> 16_000_000L // ~16ms (60Hz)
        }
        
        val predictedEvent = predictor.predict(offsetNs)

        predictedEvent?.use { predicted ->
            var predStylusIndex = -1
            for (i in 0 until predicted.pointerCount) {
                if (predicted.getToolType(i) == MotionEvent.TOOL_TYPE_STYLUS) {
                    predStylusIndex = i
                    break
                }
            }
            if (predStylusIndex == -1) return

            for (h in 0 until predicted.historySize) {
                val ghostPoint = StylusPoint(
                    x = toCanonicalX(predicted.getHistoricalX(predStylusIndex, h)),
                    y = toCanonicalY(predicted.getHistoricalY(predStylusIndex, h)),
                    pressure = predicted.getHistoricalPressure(predStylusIndex, h).coerceIn(0f, 1f),
                    tilt = predicted.getHistoricalAxisValue(MotionEvent.AXIS_TILT, predStylusIndex, h),
                    orientation = predicted.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, predStylusIndex, h),
                    timestamp = predicted.getHistoricalEventTime(h) * MS_TO_NS
                )
                onGhostPointReceived(ghostPoint)
            }

            val ghostPoint = StylusPoint(
                x = toCanonicalX(predicted.getX(predStylusIndex)),
                y = toCanonicalY(predicted.getY(predStylusIndex)),
                pressure = predicted.pressure.coerceIn(0f, 1f),
                tilt = predicted.getAxisValue(MotionEvent.AXIS_TILT, predStylusIndex),
                orientation = predicted.getAxisValue(MotionEvent.AXIS_ORIENTATION, predStylusIndex),
                timestamp = predicted.eventTime * MS_TO_NS
            )
            onGhostPointReceived(ghostPoint)
        }
    }

    private fun updateHzProfile(event: MotionEvent) {
        if (detectedHardwareHz != null) return
        if (timestampDeltasSamples.size >= MAX_PROFILE_SAMPLES) return

        val currentTs = event.eventTime * MS_TO_NS
        if (lastEventTimestampNs != 0L) {
            val delta = currentTs - lastEventTimestampNs
            if (delta > 0L) timestampDeltasSamples.add(delta)
        }
        lastEventTimestampNs = currentTs

        if (timestampDeltasSamples.size >= MAX_PROFILE_SAMPLES) {
            detectedHardwareHz = profileHardwareHz(timestampDeltasSamples)
        }
    }

    private fun profileHardwareHz(deltas: List<Long>): Int {
        if (deltas.isEmpty()) return 60
        val avgDeltaSec = deltas.average() / 1e9
        val estimatedHz = (1.0 / avgDeltaSec)
        return when {
            estimatedHz >= 180.0 -> 240
            estimatedHz >= 90.0  -> 120
            else                 -> 60
        }
    }

    private fun feedToPredictor(event: MotionEvent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            motionPredictor?.record(event)
        }
    }

    private fun resetStrokeState() {
        if (detectedHardwareHz == null) {
            lastEventTimestampNs = 0L
        }
        resetPredictorIfAvailable()
    }

    private fun resetPredictorIfAvailable() {
        // MotionPredictor, yeni bir ACTION_DOWN serisiyle record() edildiğinde
        // tamponunu otomatik sıfırlar. Explicit reset API'si gerekmez.
    }

    fun cleanup() {
        pendingSingleClickJob?.cancel()
    }
}
```

---

## ⛔ YAPAY ZEKA VE GELİŞTİRİCİLER İÇİN KESİN KURALLAR (YASAK LİSTESİ)

1. **Filtreleme YASAĞI:** Kotlin tarafında (bu dosyada) kalem titremelerini önlemek için Kalman filtresi, Low-Pass filtre veya Bezier yumuşatması KESİNLİKLE yazılmayacaktır. Yumuşatma C++ tarafına aittir.
2. **Segmentasyon YASAĞI:** Kalem mikro kesintiye uğrarsa (havaya kalkıp geri inerse) analiz yapılmayacaktır. Olaylar donanımdan nasıl geldiyse öyle iletilecektir.
3. **Ham Piksel YASAĞI:** Koordinatlar `toCanonicalX/Y` metodu kullanılarak sanal kağıt formatına oranlanmalıdır. Doğrudan `event.x` veya `event.y` kullanımı YASAKTIR.
4. **API Guard İhlali YASAĞI:** `Build.VERSION_CODES.O` (API 26) kontrolü geriye dönük crashleri önler, KESİNLİKLE kaldırılmayacaktır.
```
