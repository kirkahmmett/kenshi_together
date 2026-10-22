# 3 oyuncu desteği - tasarım taslağı

Durum: **taslak, onay bekliyor.** Kod değişikliği yok. Hedef: host + 2 join, sonra 4.

## Bugünkü model (koddan doğrulandı)

- Listen server: host bir oyuncu, otoriter. Yıldız topoloji, ENet; altta UDP veya Steam P2P.
- Host gönderirken zaten `enet_host_broadcast` kullanıyor; ENet 8 bağlantı kabul ediyor
  (`NetLink.cpp:405`).
- Paketlerin çoğunda `ownerId` (yazar) alanı var; birçok harita `(ownerId, ...)` anahtarlı
  (dünya eşyaları, hücre claim'leri). Yani wire formatı çok oyunculuya kısmen hazır.
- 3. oyuncu bağlanınca kod bilerek hata basıyor: "3+ players unsupported" (`NetLink.cpp:567`).

## Engeller (öncelik sırasıyla)

| # | Engel | Yer | Çözüm |
|---|---|---|---|
| 1 | **Steam tüneli tek eş** | `SteamP2P` ("ONE tunnel peer") | SteamID <-> sahte ENet adresi tablosu; host gelen oturumları kabul eder (yalnız Steam arkadaşları) |
| 2 | **Relay yok**: join'in paketleri yalnız host'a gider | `NetLink` alım döngüsü | Host, join kaynaklı durum paketlerini gönderen hariç diğer join'lere aynen iletir (yazar `ownerId` korunur) |
| 3 | **Ayrılma tek eşli** | `g_peerPresent` (bool), join'de `pushLeave(OWNER_ID_ALL)`, `sessionResetForUi` | Eş sayacı; ayrılan oyuncunun proxy'lerini yalnız onun `ownerId`'siyle temizle; yeni `PKT_PEER_LEFT` ile diğer join'lere bildir |
| 4 | **Sekme sahipliği** sabit `host {0}, join {1}` | `OwnRanks.h` | rank = oyuncu id (join1 -> 1, join2 -> 2); WELCOME'da id gelince yeniden çöz |
| 5 | **NPC otoritesi iki kişilik** | `ReplicatorAuthority`: tek `censusOwner_`, "host wins" çekişme kuralı | Sahip başına census; çekişmede en düşük id kazanır. **Bkz. karar 1** |
| 6 | **Kayıt/yükleme koordinasyonu** | `Plugin.cpp` LOAD_GO/NACK/ACK (tek `lastAckOk`) | Join başına ACK/NACK takibi; aktarım yalnız NACK eden join'e |
| 7 | **Bağlanınca kayıt itme** | `armConnectPush` | Yeni katılan herkesin yeniden yüklenmesine yol açar. **Bkz. karar 2** |
| 8 | **Panel** | tek "Friend's Steam ID" | Join: host ID'si (aynı). Host: oyuncu listesi; ID yapıştırma gerekmez (arkadaş kabulü) |
| 9 | **Test sistemi** iki pencere | `CoopHarness`, `run_test` | 3. Kenshi kopyası + 3 oyunculu senaryolar |

Ek bulgu (ayrı iş): oturum açıkken rol değiştirmek paneli yanıltıyor; ID yapıştırılmadan ONLINE
olunca sessizce UDP'ye düşülüyor; Steam daveti tıklanınca hiçbir şey olmuyor.

## Senin kararın gereken noktalar

1. **NPC otoritesi (3 oyuncu dağılırsa).**
   - (a) Hücre otoritesini N kişiye genişlet: oyuncular ayrı yerlerdeyken en iyisi, en riskli
     iş (bu kod iki kişide bile en çok hata çıkaran kısım).
   - (b) İlk sürümde 3. oyuncu için hücre otoritesi kapalı: NPC'leri host yönetir. Daha basit;
     ama join'in uzak bölgelerinde NPC davranışı/akıcılık kötüleşebilir (doğrulanmadı).
   - Öneri: (b) ile başlayıp çalışan 3 kişilik oyun elde etmek, sonra (a).
2. **Oturum sürerken yeni oyuncu katılınca.** Basit yol: herkes host'un kaydına birlikte yeniden
   yüklenir (5-10 sn kesinti). Zor yol: sadece yeni gelen yüklenir. Öneri: basit yol.
3. **Başlangıç kaydı.** 3 squad sekmeli başlangıç ("Wanderer x3") mod dosyasına eklenmeli; o
   zamana kadar oyunda karakterleri 3 sekmeye elle ayırmak gerekir.

## Aşamalar

| Aşama | İş | Bitti sayılır |
|---|---|---|
| A | Ağ: çok eşli Steam tüneli + relay + `PKT_PEER_LEFT` + eş sayacı (protokol 57) | 3 istemci bağlanır, hareket/envanter 3 ekranda görünür |
| B | Sahiplik ve ayrılma: rank = id, oyuncu başına temizlik | Biri çıkınca diğer ikisi etkilenmez |
| C | Kayıt/yükleme: join başına ACK/NACK | Host kaydı 2 join'e aynı anda gider |
| D | NPC otoritesi (karar 1'e göre) | `npc_sync` benzeri senaryo 3 kişide geçer |
| E | Panel, başlangıç kaydı, kurulum | Arkadaşlarla 3 kişilik oturum |

Her aşama sonunda: smoke regress (2 kişi bozulmamalı) + yeni 3 kişilik senaryolar.

Diğer projelerden gelen ekler (ayrıntı: `RESEARCH_OTHER_PROJECTS.md`):
- **B:** kısmi ayrılmada yalnız ayrılan göndericinin sıra/tekrar korumalarını sıfırla;
  oyuncu id'si yeniden kullanıldığı için bu zorunlu (KenshiCoop4up #50, #54).
- **C:** READY bariyeri - host, `LOAD_GO` sonrası tüm join'ler hazır olana kadar duraklar.

## Aşama A durumu (2026-10-22)

Yapıldı (protokol 57, dal `feature/3-players`): çok eşli Steam tüneli (host ID istemeden
kabul eder), host relay'i, en düşük boş oyuncu id'si, `PKT_PEER_LEFT`, join'de sekme =
oyuncu id'si, join'de "başka join ayrıldı" artık oturumu bitirmiyor. Ek olarak kayıt
alımındaki kuyruk yarışı düzeltildi.

Doğrulandı (tek PC, UDP, 3 istemci): ikisi de bağlandı (id 1 ve 2), join2 relay ile
join1'in istatistik/envanterini aldı (join2 ~43 paket/sn, join1 ~25), join2 ayrılınca
join1 bağlı kaldı. Test kopyası (`Kenshi-Join2`, junction'lı `data`, 375 MB) çalışıyor;
üç Kenshi için test kopyalarında grafik düşürüldü (aksi halde GPU `DEVICE_REMOVED`).

Doğrulanmadı: hareketin üçüncü ekranda görünümü, 3 kişide Steam, ayrılınca per-owner temizlik
(şimdilik ayrılmada tüm proxy'ler sıfırlanıp yeniden kuruluyor - Aşama B).

## Test altyapısı ihtiyacı

- 3. kurulum `C:\Users\pc\Kenshi-Join2`: tam kopya **~12 GB** (diskte 54 GB boş). Alternatif:
  `data` klasörünü symlink yapıp yalnız değişen dosyaları kopyalamak (birkaç yüz MB) -
  çalışıp çalışmadığı denenmeli.
- Bu makine: 16 GB RAM, RTX 4060. Üç Kenshi aynı anda zorlayabilir; düşük grafik + küçük
  pencereyle denenecek.
- Gerçek dünya testi: 3 kişi Steam üzerinden.
