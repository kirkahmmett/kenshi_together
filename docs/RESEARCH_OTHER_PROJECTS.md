# Diğer Kenshi multiplayer projeleri - öğrenilecekler

Tarih: 2026-10-22. Kaynaklar aşağıda; erişilemeyenler açıkça belirtildi.

## Özet tablo

| Proje | Model | Oyuncu | Lisans | Durum (incelendiği gün) |
|---|---|---|---|---|
| **KenshiCoop4up** (kotetsyy) | Bizim gibi nhoral/KenshiCoop çatalı; host otoriter, yıldız | 4 (host + 3) | AGPL-3.0 | Aktif (son push 2026-10-18), protokol 82/83. **3-4 kişi "uygulandı ama gerçek oturumda doğrulanmadı"** |
| **nhoral/KenshiCoop** (upstream) | Listen server, RE_Kenshi plugin | 2 | AGPL | Son commit 2026-08-10 (v0.51) |
| **Kenshi-Online** (The404Studios) | Ayrı **dedicated server** (`KenshiMP.Server.exe`), injector | 2-16 | MIT | Alpha 0.3.0, son push 2026-06-04. Envanter, hasar ve AI senkronu **yok** |
| **Kenshi Multiplayer by Anarchy** | Dedicated server, kalıcı hesaplar, kapalı lisans | ? | Kapalı | **İncelenemedi**: Nexus 403, Steam 429 verdi |
| **Kenshi Unofficial Multiplayer Mod** (eski) | ? | ? | ? | **İncelenemedi**: fandom 402 verdi |

## KenshiCoop4up'tan öğrenilenler (en değerli kaynak - aynı kod tabanı)

Bizim 3 oyuncu planımızla **aynı sonuca bağımsız olarak varmışlar**: host, join kaynaklı durumu
yazar `ownerId`'si korunarak diğer join'lere iletiyor (relay); NPC census'u her sahip
yayınlıyor, host misafir census'unu orijinal sahibiyle relay ediyor. Planımızın yönü doğru.

Doğrudan alınacaklar (öncelik sırasıyla):

| # | Ders (kaynak: `docs/REPLICATION_PITFALLS.md`) | Bizdeki karşılığı | Ne zaman |
|---|---|---|---|
| 1 | **#19 Kayıt alımında kuyruk yarışı.** `pumpSaveReceive()` BEGIN/FILE/DONE kuyruklarını ayrı ayrı boşaltıyor; son FILE parçaları DONE'dan sonra işlenirse aktarım `badCrc` ile düşüyor. Çözüm: üç kuyruğu tek kilitte birlikte al. | **Bizde aynı kod var** (`Plugin.cpp pumpSaveReceive`). Gerçek bir veri kaybı riski. | Hemen (küçük iş) |
| 2 | **READY bariyeri.** Host, `LOAD_GO` sonrası her join "dünyam hazır" (READY) diyene kadar simülasyonu duraklatıyor; geç gelen join çalışan dünyada geride kalmıyor. | "Katılımda herkes yeniden yüklenir" kararımızın eksik parçası. | Aşama C |
| 3 | **#50, #54 Kısmi ayrılma.** Bir join çıkıp başkası kalınca yalnız **o göndericinin** sıra/tekrar korumaları sıfırlanmalı; yeniden kullanılan oyuncu slotu sayaçlarını 1'den başlatır ve eski `seqSeen` onu reddeder. | Biz id'leri yeniden kullanıyoruz (protokol 57) -> **aynı tuzak bizde var**. | Aşama B |
| 4 | **#55 "Bağlandı" ile "host kabul etti" ayrı.** LOAD_GO, host kabulünden sonra gönderilmeli. | Bizde HELLO->WELCOME sonrası `pushConnect`; kontrol edilecek. | Aşama C |
| 5 | **#17 Bölünmüş otorite tam olmalı.** Hücre otoritesi: oyuncular aynı hücredeyse host yönetsin (collapse), ayrıysa her biri kendi hücresini. Komşu hücre ≠ aynı yer. | Bizde `cellCollapse=1` zaten var; 3 kişide aynı kural. | Aşama D |
| 6 | **Kanallar arası öncelik / backpressure.** Güvenilir kontrol > durum, durum:bulk = 2:1; 100 Hz durum akışı kayıt aktarımını aç bırakmamalı. | Bizde ENet kanalları var ama öncelik/backpressure yok; 3 kişide host upload'ı artıyor. | Ölçüm sonrası |
| 7 | **Join logunu host'a aynalama** (yalnız test derlemesi). | Uzak testlerde arkadaşın logunu istemek zorunda kalıyoruz. | Kolay kazanım |
| 8 | **#16 İki yazarlı değer snapshot ile replike edilemez** (para: delta + ack). | Bizde zaten delta modeli var (protokol 52); 3 kişide 2 join'in deltası host'ta toplanmalı - kontrol. | Aşama B |
| 9 | Ağ katmanı: legacy `ISteamNetworking` yerine **GameNetworkingSockets** (şifreli, lane'li). | Bizim tünel legacy API üzerinde; çalışıyor ama Valve'ın eski API'si. | Uzun vade |

Lisans notu: iki proje de AGPL; kod alınırsa kaynak belirtilmeli.

## Kenshi-Online'dan öğrenilenler

- Farklı yaklaşım: **ayrı sunucu süreci**, "sunucu gerçeği tutar, istemci girdiyi". 16 oyuncuya
  kadar konum/ölüm/bina/faction senkronu var, ama **envanter, hasar ve AI senkronu yok** - yani
  oyuncu sayısını, oyun mekaniklerinin derinliği pahasına artırmışlar. Bizim host-otoriter,
  derin senkron yaklaşımımız 3-4 kişi hedefi için daha uygun.
- Alınabilecek fikirler: geç katılanlar için **DeferredSpawnQueue** (karakter yüklenmemişse
  sonra yarat), **"ghost control"a karşı nesil (generation) takibi**, sunucu tarafında her komutun
  doğrulanması.

## Açık kalanlar

- Anarchy mod'u (Nexus 2183, Workshop 3808257125) ve eski Unofficial Multiplayer sayfası
  otomatik erişime kapalıydı; tarayıcıyla elle bakılabilir.

## Kaynaklar

- https://github.com/kotetsyy/KenshiCoop4up (README, docs/REPLICATION_PITFALLS.md, docs/API_REFERENCE.md)
- https://github.com/nhoral/KenshiCoop
- https://github.com/The404Studios/Kenshi-Online
- https://www.nexusmods.com/kenshi/mods/2183 (403)
- https://steamcommunity.com/sharedfiles/filedetails/?id=3808257125 (429)
- https://kenshi.fandom.com/wiki/Kenshi_Unofficial_Multiplayer_Mod (402)

## Brutalizer - "[BETA] KenshiCoop - Multiplayer (up to 8 players)" (Steam Workshop)

Incelendi 2026-10-23: https://steamcommunity.com/sharedfiles/filedetails/?id=3822227842
(kaynak: https://github.com/Savas07/KenshiCoop). Ayni nhoral tabanindan; adi bizimle ayni.
Son surum v0.58.0 (21 Eki). Workshop'tan abone olunarak kurulur; RE_Kenshi ayrica Nexus'tan.

Akisi: ana menu > MULTIPLAYER > HOST GAME > INVITE; arkadaslar daveti kabul eder ya da
MULTIPLAYER'da host'un yanindaki JOIN. Host NEW/LOAD GAME der, lobidekiler dunyaya cekilir;
yeni oyunda her katilan kendi karakterini normal editorde yapar. Surum lobinin sag ustunde.
F2 kucuk panel (durum, davet, cik); LAN/IP F2 > Advanced.

Iyi yaptiklari (ogrenilecek):
- Dunya yuklenmeden ONCE ana menude lobi; herkes birlikte yeni oyuna girer.
- Yeni oyunda her oyuncu KENDI karakterini yaratir (editor).
- Tuccar kilidi: bir tuccarla ayni anda tek oyuncu takas yapar, stok/para devredilir
  (bizim market senkronu sorununa basit bir cozum).
- Ceset yagmasi kopyasi, arastirma kitabi, kayit aktarimi duzeltmeleri; Join Game rich presence.
- Lobide surum gosterimi.

Kotu / yorumlardaki sikayetler (bizim de dikkat etmemiz gerekenler):
- Takasta esya kopyalanmasi/silinmesi: baskasinin envanterinden almak yerel kopyadan oluyor
  (bizim "almayi engelle" isimiz tam bunu kapatiyor).
- Mob kopyalari, bir oyuncunun gorup digerinin gormedigi NPC'ler, takilan kavgalar.
- Tuccar stogu ilk etkilesene baglaniyor, digerleri 0 para/esya goruyor; bir tuccar crash.
- Yere birakilan esya digerinde gorunmuyor; dunyada dizili esyalar kopyalaniyor.
- Kapi muhafizinin baskasina diyalogu herkese aciliyor.
- Irk/parca senkronu eksik (iskelet -> Shek).
- Maden cevheri 2-3 kat geliyor; client'in squad'a aldigi kisi host'ta gorunmuyor.
- Mod kontrolu yok (herkes ayni mod/sira olmali); RE_Kenshi'ye guvensizlik ("malware").
- Kisayollar (F1-F3) host'ta calismiyor.

Istekler: hiz artirma herkes ayni hizdaysa olsun; sunucu (dedicated) sistemi; mod kontrolu.

Bizim fark yaratabilecegimiz yerler:
- Tek tikla kurulum: RE_Kenshi'yi de bizim kurucu indirip dogruluyor (onlarda elle Nexus).
- Away modu, host izinleri, almayi engelleme, dupe engelleri, yedekleme, Turkce rehber.
- Kendi adimiz ve gorunumumuz (ad ayni - degistirilmeli).
- Mod listesi kontrolu (onlarda yok) ve surum uyusmazliginda acik uyari.
