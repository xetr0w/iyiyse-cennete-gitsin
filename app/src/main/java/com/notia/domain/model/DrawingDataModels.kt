package com.notia.domain.model

import kotlinx.collections.immutable.PersistentList
import kotlinx.collections.immutable.persistentListOf
import java.util.UUID

// ─────────────────────────────────────────────────────────────────────────────
// 1. TOOL TYPE (Araç Kutusu)
// Neden String değil de Enum? Çünkü kod yazarken "kalem" yerine "klm" yazarsak 
// sistem çöker. Enum kullanarak seçenekleri kilitliyor ve hatayı sıfırlıyoruz.
// ─────────────────────────────────────────────────────────────────────────────
enum class ToolType {
    BALLPOINT_PEN,  // Tükenmez kalem (Sadece basınca duyarlı)
    FOUNTAIN_PEN,   // Dolma kalem (Basınca ve kalemin eğimine duyarlı)
    HIGHLIGHTER,    // Fosforlu kalem (Yazının arkasını gösterir)
    MARKER,         // Kalın uçlu keçeli kalem
    ERASER          // Silgi (Pikselleri değil, matematiksel vektörü siler)
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. STYLUS POINT (Çizimin Atomu)
// Kalemin ekrana değdiği o saliselik andaki tüm fiziksel veri.
//
// Neden x ve y Canonical (Oransal)? Tabletlerin ekran çözünürlükleri farklıdır. 
// Fiziksel pikselleri kaydedersek, not başka bir cihazda bozuk görünür. 
// ─────────────────────────────────────────────────────────────────────────────
data class StylusPoint(
    val x: Float,            // Sanal kağıt üzerindeki X koordinatı
    val y: Float,            // Sanal kağıt üzerindeki Y koordinatı
    val pressure: Float,     // Kaleme ne kadar sert basıldığı (0.0 ile 1.0 arası)
    val tilt: Float,         // Kalemin yataylık açısı (Gölgeleme için)
    val orientation: Float,  // Kalemin yönü
    val timestamp: Long      // Nanosaniye cinsinden kesin zaman
)

// ─────────────────────────────────────────────────────────────────────────────
// 3. STROKE (Çizgi - Dokular)
// Yüzlerce StylusPoint'in birleşerek oluşturduğu tek bir çizgi hareketi.
//
// DİKKAT: 'points' değişkeni KESİNLİKLE standart List olmalıdır.
// Neden PersistentList değil? Çünkü Stroke tamamlandığında (endStroke) noktalar 
// bir kez yazılır ve bir daha asla mutate edilmez. Trie yapısı kurmak belleği 
// gereksiz yorar ve render anındaki okuma hızını (O(1) dizilimini) bozar.
// ─────────────────────────────────────────────────────────────────────────────
data class Stroke(
    val id: String = UUID.randomUUID().toString(), // Dünyada eşi olmayan kimlik numarası
    val toolType: ToolType,          // Hangi kalemle çizildi?
    val color: Int,                  // Rengi ne?
    val baseWidth: Float,            // Kalınlığı ne kadar?
    val points: List<StylusPoint>,   // Hangi noktalardan geçti? (STANDART LIST - O(1) Read)
    val createdAt: Long,             // Ne zaman çizilmeye başlandı?
    val version: Long = System.nanoTime(), // Çakışma çözümünde "En son yazan kazanır" kuralı
    val deviceId: String,            // Hangi cihaz/kullanıcı çizdi? (CRDT için zorunlu)
    val isDeleted: Boolean = false,  // Silindi mi? (Soft-delete)
    val textMetadata: String? = null, // İleride yapay zeka el yazısını okursa buraya yazacak
    
    // ── LASSO VE SÜRÜKLEME OPTİMİZASYONU (Sıfır Gecikme) ──
    // Sürükleme esnasında binlerce noktanın koordinatını değiştirmek cihazı dondurur.
    // Kullanıcı UI'da Lasso ile sürüklerken O(1) maliyetle offset güncellenir,
    // C++ Render Engine'e "bu çizgiyi çizerken şu kadar kaydır" denilir.
    val transformOffsetX: Float = 0f, 
    val transformOffsetY: Float = 0f
)

// ─────────────────────────────────────────────────────────────────────────────
// 4. LAYER (Katman - Organlar)
// Tıp anatomi görselleri gibi karmaşık yapılarda, resmin üzerine direkt çizmemek
// ve ayrı bir şeffaf katmanda çalışmak için kullanılır.
// ─────────────────────────────────────────────────────────────────────────────
data class Layer(
    val id: String = UUID.randomUUID().toString(),
    val title: String,               // Örn: "Kemikler", "Sinirler", "Benim Notlarım"
    val strokes: PersistentList<Stroke> = persistentListOf(), // Sık ekleme/çıkarma için GC Korumalı
    val isVisible: Boolean = true,   // Göz simgesi (Katmanı gizle/göster)
    val isLocked: Boolean = false    // Kilit simgesi (Yanlışlıkla silmeyi önle)
)

// ─────────────────────────────────────────────────────────────────────────────
// 5. PAGE (Sayfa - Sistem)
// 
// Neden widthPx ve heightPx sabit (2000x3000)? Çünkü her ekran farklıdır ama
// kağıdın boyutu standart olmalıdır (A4 kağıdı gibi). Her şey bu kağıda göre oranlanır.
// ─────────────────────────────────────────────────────────────────────────────
data class Page(
    val id: String = UUID.randomUUID().toString(),
    val pageNumber: Int,
    val widthPx: Float = 2000f,      // Sanal kağıt genişliği (Değişmez kural)
    val heightPx: Float = 3000f,     // Sanal kağıt yüksekliği (Değişmez kural)
    val version: Long = System.nanoTime(), // Sayfada bir şey değişirse nanosaniye güncellenir
    val layers: PersistentList<Layer> = persistentListOf(Layer(title = "Default")) // GC Korumalı
)

// ─────────────────────────────────────────────────────────────────────────────
// 6. NOTEBOOK (Defter - Organizma)
// Uygulamanın en üst düzey iskeleti. Defterler kendi içinde sayfaları barındırır.
// ─────────────────────────────────────────────────────────────────────────────
data class Notebook(
    val id: String = UUID.randomUUID().toString(),
    val title: String,               // Defterin adı
    val pages: PersistentList<Page> = persistentListOf(), // GC Korumalı
    val createdAt: Long = System.nanoTime(),
    val lastModifiedAt: Long = System.nanoTime(),
    val coverColor: Int              // Defter kapağının rengi
)
