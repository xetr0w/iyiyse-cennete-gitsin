# PHASE_1_DATA_MODEL.md

## Amaç

Bu doküman, projenin çekirdek veri modelini tanımlar. Tüm modeller:
- **Immutable** (Kotlin `data class` + `val` only)
- **Event-sourced** uyumlu (hiçbir model kendi içinde mutate edilmez)
- **CRDT-safe** (her varlık UUID bazlı, timestamp nanosaniye cinsinden, sürüm kontrollü)
- **Device-Agnostic** (koordinatlar fiziksel piksellerden bağımsız, Canonical Canvas yapısında)

Hiçbir model doğrudan değiştirilmez. Tüm değişiklikler command pattern üzerinden geçer.

> **Kapsam Notu:** Bu dosya yalnızca çizim pipeline'ının çekirdek domain modellerini içerir.
> Kullanıcı arayüzü tercihleri (`ToolPreset` gibi) bu dosyaya ait değildir; Phase 4'te tanımlanır.

---

## Dosya

**Path:** `app/src/main/java/com/yourdomain/notes/domain/model/DrawingDataModels.kt`

```kotlin
package com.yourdomain.notes.domain.model

import java.util.UUID

// ─────────────────────────────────────────────────────────────────────────────
// TOOL TYPE
//
// Yeni bir tool eklenirken sadece buraya yeni bir enum sabiti eklenir.
// Mevcut sabitler HİÇBİR ZAMAN yeniden adlandırılmaz veya silinmez;
// bu değerler ileride bulut veritabanında ve CRDT log'larında saklanacak.
// ─────────────────────────────────────────────────────────────────────────────

enum class ToolType {
    BALLPOINT_PEN,  // Tükenmez — basınç duyarlı genişlik, sabit opaklık
    FOUNTAIN_PEN,   // Dolma kalem — basınç + eğim (tilt) duyarlı
    HIGHLIGHTER,    // Fosforlu — sabit genişlik, Multiply blend modu
    MARKER,         // Marker — geniş, sabit opaklık
    ERASER          // Silgi — vektörel soft-delete, fiziksel silme yapmaz
}

// ─────────────────────────────────────────────────────────────────────────────
// STYLUS POINT
//
// Tek bir dokunma noktasını temsil eder. Donanımdan gelen tüm fiziksel
// veriyi taşır. C++ katmanına JNI üzerinden FloatArray + LongArray olarak
// düzleştirilip (flattening) iletilir.
//
// [x], [y] KESİNLİKLE fiziksel cihaz pikselleri DEĞİLDİR.
//          Page modelindeki widthPx / heightPx sınırlarına göre
//          oranlanmış Canonical koordinatlardır. (Örn: 0..2000f arası)
//          Bu sayede farklı çözünürlüklerde çizimler kaymaz.
//          Dönüşüm input katmanında yapılır:
//            canonicalX = rawX * (widthPx / screenWidthPx)
//            canonicalY = rawY * (heightPx / screenHeightPx)
//
// [pressure] 0.0f – 1.0f aralığında normalize edilmiş basınç değeri.
//            Ham hardware değeri değil; input katmanında normalize edilerek gelir.
//
// [tilt]     Kalemin ekrana göre eğim açısı (radyan).
//            0 = dik tutuş, π/2 = tamamen yatık.
//            FountainPenRenderer bu değeri kullanır. Desteklemeyen
//            donanımlarda 0.0f iletilir.
//
// [orientation] Kalemin ekran düzlemindeki yön açısı (radyan).
//               S-Pen gibi gelişmiş donanımlarda anlam taşır.
//               Desteklemeyen donanımlarda 0.0f iletilir.
//
// [timestamp] KESİNLİKLE nanosaniye cinsinden olmalıdır.
//             Kaynak: MotionEvent.getEventTime() * 1_000_000L
//             Milisaniye kullanmak CRDT çakışmalarına ve
//             hatalı hız hesabına yol açar — YAPILMAZ.
// ─────────────────────────────────────────────────────────────────────────────

data class StylusPoint(
    val x: Float,            // 0..widthPx arası canonical koordinat
    val y: Float,            // 0..heightPx arası canonical koordinat
    val pressure: Float,     // 0.0f – 1.0f normalize basınç
    val tilt: Float,         // radyan; desteklenmeyen donanımda 0.0f
    val orientation: Float,  // radyan; desteklenmeyen donanımda 0.0f
    val timestamp: Long      // nanoseconds — MUST NOT be changed to milliseconds
)

// ─────────────────────────────────────────────────────────────────────────────
// STROKE
//
// Tek bir kesintisiz çizim hamlesi. CRDT ve senkronizasyonun temel birimidir.
//
// [id] UUID v4 string. Üretim mantığına DOKUNULMAZ.
//      Bulut merge operasyonları ve CRDT log'u bu ID'ye bağlıdır.
//
// [createdAt] Nanosaniye. Stroke'un ilk StylusPoint'i alındığı an atanır.
//             Bir kez set edilir, sonradan değiştirilmez.
//
// [version] CRDT çakışma çözümü (Last Write Wins) için kullanılır.
//           ZORUNLU KURAL: her copy() çağrısında açıkça
//           version = System.nanoTime() olarak set edilmelidir.
//           Aksi takdirde CRDT çakışma çözümü kör kalır.
//           Örnek:
//             stroke.copy(isDeleted = true, version = System.nanoTime())
//
// [isDeleted] Soft-delete flag. Stroke fiziksel olarak silinmez;
//             komut katmanı bu flag'i true yapan yeni bir kopya üretir (copy()).
//             Bu sayede CRDT undo/redo log'u bozulmaz.
//             isDeleted = true olan stroke'lar render katmanında atlanır.
//
// [points] Immutable liste. Stroke commit edildikten sonra nokta eklenemez.
//          Aktif çizim sırasında geçici buffer input katmanında tutulur;
//          ACTION_UP ile stroke tamamlandığında bu model oluşturulur.
//
// [color] ARGB int. Örnek: 0xFF000000.toInt() → opak siyah.
//
// [baseWidth] Piksel cinsinden temel kalem genişliği (canonical birimde).
//             Pressure ve tilt çarpanları C++ katmanında uygulanır, burada saklanmaz.
//
// [textMetadata] OCR veya yapay zeka katmanının bu stroke için ürettiği
//               tanıma verisi. İleride doldurulacak; şimdilik null kalır.
// ─────────────────────────────────────────────────────────────────────────────

data class Stroke(
    val id: String = UUID.randomUUID().toString(),
    val toolType: ToolType,
    val color: Int,
    val baseWidth: Float,
    val points: List<StylusPoint>,
    val createdAt: Long,             // nanoseconds — MUST NOT be changed to milliseconds
    val version: Long = System.nanoTime(),
    val isDeleted: Boolean = false,
    val textMetadata: String? = null
)

// ─────────────────────────────────────────────────────────────────────────────
// LAYER
//
// Karmaşık çizimlerde Z-Index (derinlik) yönetimini sağlar.
// Tıp dersleri gibi anatomik şemalar üzerinde ayrı katmanlarda çalışmayı mümkün kılar.
//
// [strokes] Immutable liste. Layer değiştirilmek istendiğinde
//           komut katmanı copy() ile yeni bir Layer üretir.
//
// Z-INDEX KURALI: Layer'ın derinlik sırası (Z-Index) 'order: Int' gibi
// ayrı bir alan ile belirlenmez. Page.layers listesi içindeki indeks
// pozisyonu Z-Index'tir. Bu kural, liste indeksi ile ayrı bir alan
// arasındaki potansiyel çakışmayı (conflict) önler.
// ─────────────────────────────────────────────────────────────────────────────

data class Layer(
    val id: String = UUID.randomUUID().toString(),
    val title: String,
    val strokes: List<Stroke> = emptyList(),
    val isVisible: Boolean = true,
    val isLocked: Boolean = false
)

// ─────────────────────────────────────────────────────────────────────────────
// PAGE
//
// Bir not sayfasını ve onun fiziksel sınırlarını temsil eder.
//
// [pageNumber] 1-indexed. Sayfa sıralaması bu değere göre yapılır.
//
// [widthPx], [heightPx] Canonical Canvas (Sanal Sayfa) boyutları.
//             Fiziksel cihazın çözünürlüğü ne olursa olsun,
//             C++ ve veri katmanı bu sınırları baz alır.
//             Varsayılan 2000x3000: A4 oranına yakın, yeterli çözünürlük.
//
// [layers] Immutable liste. Sayfa değiştirilmek istendiğinde
//          komut katmanı copy() ile yeni bir Page üretir.
//          Listeye doğrudan eleman eklenmez/çıkarılmaz.
//          Varsayılan: tek bir "Default" katman ile başlar.
// ─────────────────────────────────────────────────────────────────────────────

data class Page(
    val id: String = UUID.randomUUID().toString(),
    val pageNumber: Int,
    val widthPx: Float = 2000f,
    val heightPx: Float = 3000f,
    val layers: List<Layer> = listOf(Layer(title = "Default"))
)

// ─────────────────────────────────────────────────────────────────────────────
// NOTEBOOK
//
// Uygulamanın en üst seviye doküman yapısı.
// Sayfaları, meta verileri ve kullanıcı tanımlı kapak rengini barındırır.
//
// [createdAt], [lastModifiedAt] KESİNLİKLE nanosaniye cinsinden olmalıdır.
//              Tüm modellerde nanosaniye tutarlılığı zorunludur.
// ─────────────────────────────────────────────────────────────────────────────

data class Notebook(
    val id: String = UUID.randomUUID().toString(),
    val title: String,
    val pages: List<Page> = emptyList(),
    val createdAt: Long = System.nanoTime(),
    val lastModifiedAt: Long = System.nanoTime(),
    val coverColor: Int
)
```

---

## CRDT ve Mimari Uyumluluk Notları

| Kural | Neden |
|---|---|
| `id` alanları değiştirilemez | Bulut merge operasyonları ID'yi primary key olarak kullanır |
| Tüm timestamp'ler nanosaniye | Milisaniye granülaritesi eş-zamanlı olayları çakıştırır |
| `copy()` çağrısında `version = System.nanoTime()` zorunlu | Aksi takdirde Last Write Wins çakışma çözümü kör kalır |
| `isDeleted` soft-delete | Fiziksel silme CRDT log'unda kayıp veri yaratır |
| Canonical koordinatlar (2000x3000) | Farklı ekran çözünürlüklerinde ve PDF export'ta veri bozulmaması |
| Layer Z-Index = liste indeksi | `order: Int` ile liste pozisyonu çakışmasını önler |
| Tüm alanlar `val` | Mutation command dışına sızmaz; snapshot tutarlılığı korunur |
| `ToolPreset` bu dosyada tanımlanmaz | UI tercih verisi; Phase 4 Tool Strategy'ye aittir |

---

## Değişiklik Kuralları

- `ToolType` sabitlerini **yeniden adlandırma veya silme** — **YASAK**
- `Stroke.id` ve `createdAt` üretim mantığını değiştirme — **YASAK**
- Herhangi bir `val` alanı `var` yapma — **YASAK**
- Herhangi bir timestamp'i nanosaniyeden başka bir birime çevirme — **YASAK**
- Koordinatları fiziksel ekran pikselleriyle (`rawX`, `rawY`) model içine kaydetme — **YASAK**
- `copy()` çağrısında `version` alanını güncellemeden geçme — **YASAK**
- Yeni tool eklemek için: `ToolType` enum'una yeni sabit + Phase 4'te yeni strategy sınıfı (mevcut renderer'lara dokunulmaz)
