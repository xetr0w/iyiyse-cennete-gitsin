# PHASE_1_DATA_MODEL.md

## Mimari Felsefe ve Amaç

Bu doküman, Notia'nın çekirdek veri modelini tanımlar. Bu modelin temel amacı, uygulamanın sadece bugün "tek cihazda" çalışmasını değil, yarın "birden fazla cihazda eşzamanlı (multiplayer)" çalışmasını sağlamaktır.

Tüm modellerimiz şu 4 temel sütuna dayanır:
1.  **Immutable (Değişmezlik):** Modeller oluşturulduktan sonra içindeki veriler değiştirilemez (`val` kullanımı). Neden? Çünkü ekran saniyede 120 kez yenilenirken verinin aniden değişmesi uygulamayı çökertir (Race Condition).
2.  **Event-Sourced (Olay Tabanlı):** Veri silinmez, "silindi" olarak işaretlenir. Neden? "Geri Al" (Undo) yapabilmek ve geçmişi kaybetmemek için.
3.  **CRDT-Safe (Çakışma Korumalı):** Aynı nota iki kişi aynı anda çizim yaparsa veriler birbirine girmesin diye her nesnenin bir zaman damgası (version) ve benzersiz kimliği (UUID) vardır.
4.  **Device-Agnostic (Cihaz Bağımsız):** Koordinatlar cihazın ekranına göre değil, "Sanal bir kağıda" göre tutulur. Neden? Notu S9+ tabletten telefona attığında çizgiler kaymasın diye.

> **Yapay Zeka Asistanları İçin Not:** Bu dosya uygulamanın anayasasıdır. Modeller doğrudan değiştirilmez, tüm state mutasyonları Command Pattern üzerinden geçmelidir. Alt kısımdaki YASAK listesine kesinlikle uyulacaktır.

---

## Modeller (Path: `app/src/main/java/com/yourdomain/notes/domain/model/DrawingDataModels.kt`)

```kotlin
package com.yourdomain.notes.domain.model

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
// Bu yüzden "2000x3000'lik sanal bir kağıdın neresinde?" sorusunu kaydederiz.
//
// Neden Milisaniye değil Nanosaniye? Çünkü hızlı bir çizimde aynı milisaniyeye
// birden fazla nokta sığabilir. Nanosaniye noktaların sırasının karışmasını engeller.
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
// Neden deviceId var? İki arkadaş aynı deftere yazarken kimin ne çizdiğini bilmek için.
// Neden isDeleted var? Çizgiyi sildiğimizde tamamen yok etmeyip 'gizleriz' ki,
// kullanıcı "Geri Al" butonuna bastığında çizgi kolayca geri gelsin.
// ─────────────────────────────────────────────────────────────────────────────
data class Stroke(
    val id: String = UUID.randomUUID().toString(), // Dünyada eşi olmayan kimlik numarası
    val toolType: ToolType,          // Hangi kalemle çizildi?
    val color: Int,                  // Rengi ne?
    val baseWidth: Float,            // Kalınlığı ne kadar?
    val points: List<StylusPoint>,   // Hangi noktalardan geçti?
    val createdAt: Long,             // Ne zaman çizilmeye başlandı?
    val version: Long = System.nanoTime(), // Çakışma çözümünde "En son yazan kazanır" kuralı için
    val deviceId: String,            // Hangi cihaz / kullanıcı çizdi?
    val isDeleted: Boolean = false,  // Silindi mi? (Soft-delete)
    val textMetadata: String? = null // İleride yapay zeka el yazısını okursa buraya yazacak
)

// ─────────────────────────────────────────────────────────────────────────────
// 4. LAYER (Katman - Organlar)
// Tıp anatomi görselleri gibi karmaşık yapılarda, resmin üzerine direkt çizmemek
// ve ayrı bir şeffaf katmanda çalışmak için kullanılır.
// 
// Neden 'order' (Sıra) adında bir değişken yok? Çünkü katmanın sırasını, 
// listenin içindeki konumu belirler. Hem order hem liste sırası olursa çakışma yaşanır.
// ─────────────────────────────────────────────────────────────────────────────
data class Layer(
    val id: String = UUID.randomUUID().toString(),
    val title: String,               // Örn: "Kemikler", "Sinirler", "Benim Notlarım"
    val strokes: List<Stroke> = emptyList(),
    val isVisible: Boolean = true,   // Göz simgesi (Katmanı gizle/göster)
    val isLocked: Boolean = false    // Kilit simgesi (Yanlışlıkla silmeyi önle)
)

// ─────────────────────────────────────────────────────────────────────────────
// 5. PAGE (Sayfa - Sistem)
// 
// Neden widthPx ve heightPx sabit (2000x3000)? Çünkü her ekran farklıdır ama
// kağıdın boyutu standart olmalıdır (A4 kağıdı gibi). Her şey bu kağıda göre oranlanır.
// 
// Neden version var? İnternet koptuğunda offline çizim yaparsan, internet geldiğinde
// binlerce noktayı tek tek kontrol etmek yerine sadece sayfa versiyonuna bakıp senkronize olmak için.
// ─────────────────────────────────────────────────────────────────────────────
data class Page(
    val id: String = UUID.randomUUID().toString(),
    val pageNumber: Int,
    val widthPx: Float = 2000f,      // Sanal kağıt genişliği (Değişmez kural)
    val heightPx: Float = 3000f,     // Sanal kağıt yüksekliği (Değişmez kural)
    val version: Long = System.nanoTime(), // Sayfada bir şey değişirse bu nanosaniye güncellenir
    val layers: List<Layer> = listOf(Layer(title = "Default"))
)

// ─────────────────────────────────────────────────────────────────────────────
// 6. NOTEBOOK (Defter - Organizma)
// Uygulamanın en üst düzey iskeleti. Defterler kendi içinde sayfaları barındırır.
// ─────────────────────────────────────────────────────────────────────────────
data class Notebook(
    val id: String = UUID.randomUUID().toString(),
    val title: String,               // Defterin adı
    val pages: List<Page> = emptyList(),
    val createdAt: Long = System.nanoTime(),
    val lastModifiedAt: Long = System.nanoTime(),
    val coverColor: Int              // Defter kapağının rengi
)
```

---

## ⛔ YAPAY ZEKA VE GELİŞTİRİCİLER İÇİN KESİN KURALLAR (YASAK LİSTESİ)

Mimarinin çökmemesi için aşağıdaki kurallara %100 uyulacaktır:

1.  **Sabitleri Değiştirme YASAĞI:** `ToolType` enum içindeki sabitler silinemez veya yeniden adlandırılamaz (Geçmiş veritabanlarıyla uyumluluğu bozar).
2.  **ID ve Tarih Oynama YASAĞI:** `Stroke.id` ve `createdAt` değerleri oluşturulduktan sonra asla değiştirilemez.
3.  **Değişmezlik (Immutability) YASAĞI:** Hiçbir modelin içinde `var` kelimesi kullanılamaz. Modeller salt okunurdur (`val`). Değişiklikler sadece `copy()` fonksiyonu ile yeni bir kopya üretilerek yapılır.
4.  **Zaman Birimi YASAĞI:** Tüm zaman damgaları (`timestamp`, `createdAt`, `version`) **nanosaniye** cinsinden olmak zorundadır. Milisaniye kullanılamaz.
5.  **Ham Piksel YASAĞI:** Ekrana dokunulan koordinatlar (`rawX`, `rawY`) modele direkt kaydedilemez. Input katmanında mutlaka `widthPx` ve `heightPx`'e göre oranlanarak (Canonical formata dönüştürülerek) kaydedilmelidir.
6.  **Çapraz Referans (Cross-Reference) YASAĞI:** Modeller arası ID referansı kullanmak yasaktır. Yapı kesinlikle `Notebook -> Page -> Layer -> Stroke` şeklinde hiyerarşik bir ağaç (Tree Structure) olarak kalmalıdır.
