# CLAUDE.md — KenshiCoop (fork)

Kenshi 1.0.65 için co-op multiplayer: RE_Kenshi'nin yüklediği bir KenshiLib plugin'i
(`KenshiCoop.dll`). Bu fork'un hedefi: **2–8 oyunculu, birkaç saatlik oturumlarda save
bozulmadan ve akıcı çalışan co-op.** Upstream: `github.com/nhoral/KenshiCoop`.

Yanıtlar Türkçe; teknik terimler İngilizce kalabilir.

## Durum (Faz 0 sonu, 2026-10-21)

- Fork: `github.com/kirkahmmett/kenshi-coop` (private). Yerel klon `C:\Users\pc\Desktop\KenshiCoop`,
  remote'lar `origin` = fork, `upstream` = `nhoral/KenshiCoop`. Upstream değişikliklerini
  `git fetch upstream` ile al; topluluk PR'larını `git fetch upstream pull/<n>/head:pr-<n>` ile incele.
- Kod tabanı upstream `main` @ `5a761e19` (v0.51, `PROTOCOL_VERSION = 55`). Tek fark: KenshiLib
  v0.4.1 ile derlenebilmesi için `EngineInternal.h`'deki `CombatClass.h` include yolu (aşağıda "KenshiLib sürümü").
  Upstream'in son commit'i 2026-08-10; bakımcı PR'ları GitHub üzerinden merge etmiyor,
  özellikleri kendi private dalında yeniden yazıp export ediyor.
- Upstream'deki `resources/` klasörü `.gitignore`'da (bakımcının yerel notları). Bu yüzden
  dokümanların işaret ettiği `resources/BUILD_SETUP.md`, `resources/PROTOCOL_HISTORY.md`,
  `resources/CODE_MAP.md` **bu repoda yok**. Derleme gereksinimleri için tek güvenilir kaynak
  `scripts/build_plugin.cmd` ve `src/plugin/KenshiCoop.vcxproj`.
- `docs/API_REFERENCE.md` §8–§13 **eski** (`NetClient`, `MainThreadQueue`, `Protocol.h` artık yok).
  Canlı karşılıkları: `net/NetLink`, `core/Inbound`, `netproto/Wire.h`, `src/plugin/test/`.
  README'de geçen `src/nettest/` da artık yok.
- **Öncelik (kullanıcı kararı, 2026-10-21): önce VANİLLA oyunda co-op.** Ek mod uyumluluğu (mod listesi
  karşılaştırması, Workshop mod testleri) şimdilik kapsam dışı. Bu makinedeki Steam hesabı 46 Workshop
  mod'una abone, ama `data\mods.cfg`'de yalnızca `KenshiCoop.mod` etkin - testler vanilla oyunda koşar.

### Bu makinedeki oyun kurulumu (2026-10-21)

- Steam'in kurduğu sürüm **1.0.68** (`kenshi_x64.exe` MD5 `8a03c256…`), KenshiCoop ise 1.0.65 için yazılı.
  RE_Kenshi 0.3.5 (GitHub'dan, `_loose` paket) iki kuruluma da kurulu: sürümü tanımayınca kendini
  `RE_Kenshi\kenshi_x64.exe`'den yeniden başlatır; o dosya, paketteki `courgette64 -apply` yamasıyla üretilmiş
  **1.0.65** exe'si (MD5 `df4a5a7e…`, RE_Kenshi `config.json` tablosuyla doğrulandı). Orijinal exe'ye dokunulmadı.
  Oyun penceresi başlığı "Kenshi 1.0.65" ise RE_Kenshi devrede demektir.
- `Plugins_x64.cfg` ve `data\mods.cfg` yedekleri: `*.pre-rekenshi.bak`, `*.pre-kenshicoop.bak`.
- Oyuncular için aynı kurulumu `scripts\installer\KenshiCoop-Setup.cmd` yapar (kit kökünde); `-DryRun`,
  `-Uninstall [-RemoveReKenshi]` destekler.

## Derleme

Toolchain, `build_plugin.cmd`'deki sabit yollardan okunuyor:

| Gereksinim | Beklenen yol |
|---|---|
| VC++ 2010 (v100) x64 `cl.exe` (Windows SDK 7.1 + KB2519277 compiler update, ya da tam VS2010) | `C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\bin\amd64` |
| Windows SDK 7.1 | `C:\Program Files\Microsoft SDKs\Windows\v7.1` |
| VC++ 2010 x86+x64 redistributable (`cl.exe` `msvcr100.dll` ister) | sistem |
| VS2022 Build Tools: **MSBuild + `Microsoft.VisualStudio.Component.VC.CoreBuildTools`** (C++ hedefleri; yalnız MSBuild yetmez, `MSB4019 Microsoft.Cpp.Default.props` verir) | `vswhere` ile bulunur |
| KenshiLib_Examples_deps (git-LFS; boost_1_60_0 + Ogre/MyGUI lib) | `third_party/KenshiLib_deps/` |
| **KenshiLib v0.4.1 SDK** (header + `KenshiLib.lib`; bkz. aşağısı) | `third_party/KenshiLib_sdk/` (git-ignored) ya da `%KENSHILIB_SDK%` |
| ENet 1.3.18 + `third_party/enet/patches/0001` (C89 for-loops) ve `0002` (Steam socket hooks), patch'ler **repo kökünden** `git apply` ile | `third_party/enet/enet/` |

```bash
cmd //c scripts\build_prototest.cmd         # dist\prototest.exe (wire-protocol unit testleri) - stok script çalışır
cmd //c scripts\build_plugin.cmd            # Harness (KenshiLib SDK seçimi aşağıda; seçtiği yolu ekrana yazar)
cmd //c scripts\build_plugin.cmd Release    # oyunculara giden DLL (senaryo kodu yok)
```

### KenshiLib sürümü (önemli)

**Yayınlanmış hiçbir KenshiLib sürümü upstream kodunu değiştirmeden derlemiyor** (2026-10-21'de doğrulandı):
- `kenshi/CombatClass.h` v0.3.4'te `kenshi/combat/` altına taşındı (v0.3.3'e kadar eski yerde).
- v0.3.0–v0.4.0 arası her sürüm `BuildingDesignation` enum'unu iki kez tanımlıyor (`Platoon.h` + `Building/Building.h`);
  proje ikisini aynı TU'da include ettiği için 72 hata çıkıyor. v0.4.1'de düzeltildi.
- `KenshiLib_Examples_deps` master'ı v0.4.0 (bozuk); onun bir önceki commit'i `e75769b` v0.3.0 (yine 72 hata).

Çözüm: **KenshiLib v0.4.1** + `EngineInternal.h`'de `#include <kenshi/combat/CombatClass.h>`. Bununla Harness ve
Release 0 hatayla derleniyor; Release DLL `KenshiLib.dll`'den yayınlanan v0.51 DLL'i ile **aynı 185 sembolü** import
ediyor ve boyutu aynı (1.027.072 byte). Bu semboller KenshiLib 0.3.0'dan beri mevcut, yani oyuncu tarafında daha yeni
bir `KenshiLib.dll` gerekmiyor (oyunda doğrulanmadı).

`build_plugin.cmd` KenshiLib header ve lib'lerini şu sırayla seçer: `%KENSHILIB_SDK%` → `third_party\KenshiLib_sdk`
→ `third_party\KenshiLib_deps` (bu sonuncusu master'da 0.4.0 olduğu için derlemez). Boost her zaman
`third_party\KenshiLib_deps\boost_1_60_0`'dan gelir. SDK kurulumu (~7 MB):
```bash
T=third_party/KenshiLib_sdk; TMP=$(mktemp -d)
git clone --depth 1 --branch v0.4.1 https://github.com/BFrizzleFoShizzle/KenshiLib.git $TMP/src
mkdir -p $T/KenshiLib/Libraries && cp -r $TMP/src/Include $T/KenshiLib/Include
cp $TMP/src/Libraries/ogre/OgreMain_x64.lib $TMP/src/Libraries/mygui/MyGUIEngine_x64.lib $T/KenshiLib/Libraries/
# KenshiLib.lib: release asset KenshiLib_v0.4.1.zip (sha256 4ab75418bd98886a06d72fbb6d92c6dc9b5c8a0881d301ef96127a4bb0a8ffa0) içinden
```

### Toolchain kurulum tuzakları (bu makinede yaşandı)

- SDK 7.1 kurulumu, VC++ 2010 redist **10.0.40219** kuruluyken başarısız olur: önce redist'leri kaldır
  (`msiexec /x {GUID}` **yönetici olarak**; winget'in sessiz kaldırması yetki olmadan 1603 verir), sonra kur, en son geri kur.
- .NET 4.8 olan sistemde SDK 7.1 sihirbazı "Visual C++ Compilers" bileşenini **sessizce atlar**. Derleyiciyi ISO'dan
  doğrudan kur: `Setup\vc_stdx86\vc_stdx86.msi`, sonra `Setup\vc_stdamd64\vc_stdamd64.msi`, ardından
  `VC-Compiler-KB2519277.exe /q` (cl.exe 16.00.40219.01 olmalı).
- SDK kurulumu redist'leri RTM sürümüne (10.0.30319) düşürür; winget "upgrade" de kaldırmaya çalışıp 1603 verir.
  winget'in indirdiği `vcredist_x64/x86.exe`'yi yönetici olarak `/q /norestart` ile çalıştır.
- `KenshiLib_deps`'teki `Setup.bat` gerekmez (kendini yükseltip `setx` ile kalıcı env yazar); `boost.zip`'i
  Windows `tar.exe` ile aç (Git Bash `tar`'ı zip açamaz).

Çıktı: `src/plugin/x64/<Config>/KenshiCoop.dll`. `deploy.cmd [KenshiDir] [Config]` bu DLL'i
`<Kenshi>\mods\KenshiCoop\` dizinine, varsa `%USERPROFILE%\Kenshi-Join\mods\KenshiCoop\` dizinine de kopyalar.
Kenshi açıkken DLL kilitli olur, deploy başarısız olur.

## Test

Hepsi tek makinede iki Kenshi kurulumu ile çalışır: host Steam kurulumunda, join
`%USERPROFILE%\Kenshi-Join` kopyasında (`scripts\setup_join_install.cmd`). Bağlantı `127.0.0.1:27800` üzerinden UDP;
**Steam hesabı gerekmez.** Her iki kurulumda da RE_Kenshi olmalı. Save'ler `fixtures/saves/` altından
`%LOCALAPPDATA%\kenshi\save` dizinine geri yüklenir.

- Bu makinede çalışıyor: `coop_presence` 2026-10-21'de PASS. Harness'i **`en-US` kültürüyle** çalıştır:
  `powershell -Command "[Threading.Thread]::CurrentThread.CurrentCulture='en-US'; & '.\scripts\regress.ps1' -SkipBuild"`.
- Testler ekranda iki Kenshi penceresi açar ve açık Kenshi işlemlerini öldürür; kullanıcıya önceden haber ver.
- Bir kurulumun ilk açılışında Kenshi "Mods warning" kutusu çıkarabilir; `start_kenshi.ps1` artık OK'a
  `PostMessage` ile basar ve bu kutuyu "Evet" ile geçer (eskiden `SendMessage` testi 14 dk kilitliyordu).
- Steam P2P ve Steam davetleri bu makinede test edilemez (iki ayrı Steam hesabı gerekir); harness hep UDP.

```powershell
# Oyunsuz katman (milisaniyeler; her değişiklikte çalıştır)
powershell -ExecutionPolicy Bypass -File scripts\tests\Contract.Tests.ps1
powershell -ExecutionPolicy Bypass -File scripts\tests\CrawlMove.Fixture.ps1
dist\prototest.exe

# Tek iterasyon: kill -> build -> deploy -> host+join -> PASS/FAIL (exit code = verdict)
powershell -ExecutionPolicy Bypass -File scripts\dev_cycle.ps1 -Save squad1 -Scenario coop_presence -Sync

# Regresyon: smoke (~15 senaryo) / full; -Only a,b ile daralt; -Variants clean,wan,wanskew
powershell -ExecutionPolicy Bypass -File scripts\regress.ps1
powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full -SkipBuild
```

Senaryo manifest'i `scripts/scenarios.psd1`'de (Save, PrimaryGate, Gating/Advisory oracle'lar, Tier).
Çıktılar `tools/test-runs/<stamp>/` altına (log, png, verdict.json), trend verisi `tools/test-runs/history.jsonl` dosyasına yazılır.

### Kurallar

- **`regress.ps1` (en az smoke tier) geçmeden iş bitmiş sayılmaz.** Protokol, inventory, save
  veya authority'ye dokunan değişikliklerde ilgili full-tier senaryoları da (`-Only`) koş.
  Toolchain/oyun yoksa bunu açıkça söyle; "derlenmedi/test edilmedi" demeden bitti deme.
- Yeni bir düzeltme, **önce düzeltme olmadan FAIL eden** bir gate ile gelir (bkz. REPLICATION_PITFALLS §7–§8).
- `FLAKY` (retry'da geçen) senaryo yeşil sayılır ama raporda belirtilir.

## Mimari özet

- **Yükleme:** RE_Kenshi `mods/KenshiCoop/RE_Kenshi.json` dosyasını okur ve `startPlugin()` çağrılır.
  Plugin `GameWorld::mainLoop_GPUSensitiveStuff` fonksiyonunu hook'lar. **Tüm oyun mutasyonu main thread'de,
  SEH (`__try/__except`) içinde yapılır.** SEH frame'lerinde unwind gerektiren C++ nesnesi olamaz.
- **Ağ (`src/plugin/net/`):** `NetLink`, ENet'i ayrı bir thread'de tek başına sahiplenir. Gelen veri
  `core/Inbound` kuyruğu ile main thread'e aktarılır, giden veri lock altında kopyalanan `queueX()` çağrılarıyla gider. Üç kanal var:
  `CH_RELIABLE` (olaylar/intent'ler), `CH_UNRELIABLE` (20 Hz entity batch, stealth, cam hint; en yeni olan kazanır),
  `CH_BULK` (save transferi). Transport UDP ya da Steam P2P'dir (ENet socket hook'ları ile tünellenir, MTU 1200).
  Host'ta `enet_host_create(..., 8 peers, ...)` çağrılır, ama **sync modeli host + TEK join**: id≥2 bağlanırsa
  sadece `netErr("3+ players unsupported ...")` loglanır. Join'in ürettiği state diğer join'lere relay edilmez,
  `OWNER_ID_ALL` sweep'leri tek peer varsayar.
- **Yetki modeli:** Dünya ve NPC'lerde host authoritative'dir. Her oyuncu kendi squad tab'ının sahibidir
  (`OwnRanks`: host=rank 0, join=rank 1). Protocol 49'dan beri **cell authority** var: 4608 u'luk hücrelerde
  NPC census'u o hücrede duran taraf yazar. İki squad aynı hücredeyse her şey host'a çöker (`KENSHICOOP_CELL_COLLAPSE`).
  Placed building'lerde placer authoritative'dir. Para tek havuzdur: host total gönderir, join delta gönderir (`PKT_MONEY_DELTA`, ack'li).
  Save/load koordinelidir: join'in yerel save'leri bastırılır, host save'i `SaveXfer` (CH_BULK, CRC tablosu) ile akar.
  Join'in yerel load'u host'a yönlendirilir; host da `PKT_LOAD_GO` + fingerprint ile yanıt verir.
- **Sync (`src/plugin/sync/`):** `Replicator` sınıfı partial-class TU'lara bölünmüştür:
  Core, Publish, Drive, Channels, Items, Spawn, Authority. `Interp` interpolasyon buffer'ıdır, `SaveXfer` ise save stream'idir.
  Kural: sürekli state unreliable gider (20 Hz, kendini iyileştirir), tek seferlik geçişler (KO/death/recruit/squad move) reliable `PKT_EVENT` ile gider.
- **Engine facade (`src/plugin/game/`):** `Engine*.cpp`. Oyun-iç header'ları yalnızca `EngineInternal.h` include eder
  (bu kuralı Contract testi zorluyor).
- **Kimlik:** `hand` (5×u32), aynı save'i yükleyen her makinede aynı nesneye çözülür. Replikasyonun temeli budur.
- **Test kodu:** `src/plugin/test/Scenario*.cpp` yalnızca Harness build'ine girer. Oracle'lar
  `scripts/CoopOracles.psm1` + `scripts/oracles/*.ps1` içinde, iki tarafın log'larını sayısal olarak karşılaştırır.
- **Yapılandırma:** `mods/KenshiCoop/coop_config.json` dosyası ve `KENSHICOOP_*` env var'ları (env her zaman kazanır).
  Kanal anahtarları ve varsayılanlar `core/Config.cpp` içinde. Rol oyun içinde F2 panelinden de seçilir.

## Protokol değişikliği kuralları

- Wire tanımı **yalnızca** `src/netproto/Wire.h` içindedir (+`ContentHash.h`). Plain C++03, `#pragma pack(1)`,
  little-endian, struct byte'ları doğrudan gönderilir.
- Wire formatını değiştiren **her** değişiklikte `PROTOCOL_VERSION` artırılır. Handshake'te sürüm farkı
  reddedilir ve geriye uyumluluk yoktur. **Sürüm değişince tüm oyuncular (host + her join) aynı build'e güncellemek zorunda.**
  Release notunda bunu açıkça yaz.
- Yeni packet tipi enum'un sonuna eklenir, mevcut numaralar asla yeniden kullanılmaz.
- `src/prototest/main.cpp` her struct'ın boyutunu ve round-trip'ini doğrular. Yeni veya değişen her struct için
  oraya assert ekle. Prototest v100 ile derlenir, çünkü layout'u bu toolchain kilitler.
- Sürüm geçmişi upstream'de `resources/PROTOCOL_HISTORY.md` dosyasında tutuluyordu ve bu dosya bizde yok.
  Bu fork'ta her bump için `docs/PROTOCOL_HISTORY.md` dosyasına giriş ekle (dosyayı ilk bump'ta oluştur).
- Upstream PR'ları farklı tabanlardan aynı numaraları kullanıyor (ör. #28 → 46, #50 → 49, #70 → 56).
  Port ederken **numarayı bizim sıramıza göre yeniden ata**, PR'daki numarayı kopyalama.

## Loglar

- `<Kenshi>\KenshiCoop_host.log` / `KenshiCoop_join.log`: plugin'in kendi logu (cwd'ye göreli, `KENSHICOOP_LOG` ile değişir).
  **Tuzak:** Dosya adı `startPlugin()` sırasında config/env'deki role göre seçilir. Oyuncu rolü F2'den JOIN yapsa bile
  dosya `KenshiCoop_host.log` adını taşır ve satırlar `[HOST]` etiketiyle başlar. Gerçek rolü `JOIN save suppression`,
  `peer connected id=` gibi satırlardan oku.
- `<Kenshi>\RE_Kenshi_log.txt`: plugin yüklendi mi (`KenshiCoop loaded!`).
- `<Kenshi>\kenshi.log` ve `kenshi_info.log`: engine (ör. `alredy has destroy reason` = çift destroy).
- Harness çıktıları `tools/test-runs/<stamp>/` altında.

### Performans satırları (`core/TickProfiler.h`)

Varsayılan olarak her 10 s'de bir (`KENSHICOOP_PERF_MS`, `0` = kapalı) iki satır:
```
[perf] win=10000ms ticks=1043 (104/s) hook=1.84/9.12ms engine=6.31/22.40ms top: pubOwned=0.52/3.10 ...
[net] stats win=10000ms peers=1 up=48.2KB/s down=12.0KB/s pkts up=210/s down=60/s rttMax=37ms lossMax=0.0%
```
`a/b` = tick başına ortalama / pencerede tek çağrının en kötüsü (ms). `hook` motor dahil tüm kanca, `engine`
Kenshi'nin kendi tick'i, `top` en pahalı 8 bölüm (isimler `perf::Section`). `[net]` ENet'in host sayaçlarından
(protokol yükü dahil) ve en kötü bağlı peer'in RTT/kayıp oranından gelir. Optimizasyon iddiası bu satırlarla
öncesi/sonrası karşılaştırılmadan yapılmaz. Yeni bir alt sistem kancaya eklenirse `KC_PERF(S_..., ...)` ile sar ve
gerekirse `Section` + `sectionName`'e satır ekle (prototest "every section named" bunu kontrol eder).

## Tuzaklar (Faz 0'da doğrulananlar)

- **Türkçe locale:** Bu makine `tr-TR`. `CrawlMove.Fixture.ps1` burada FAIL veriyor, `en-US` ile PASS veriyor.
  Sebep: fixture sahte log satırlarını PowerShell interpolation ile üretiyor ve sayılar `1,5` olarak yazılıyor.
  Harness/oracle script'lerini `en-US` kültürüyle çalıştır:
  `[Threading.Thread]::CurrentThread.CurrentCulture = 'en-US'` (ya da script'i bu ayarla saran bir wrapper kullan).
  Oracle'lardaki `-f`/`ToString`/`ToUpper` kullanımları henüz denetlenmedi.
- **Save transfer kapısı zayıf:** `SaveXfer.cpp::onSaveDone` commit'e yalnızca `fileCount` eşitliğiyle izin veriyor.
  0 dosyalı bir transfer de geçiyor ve `onSaveBegin` save adını doğrulamıyor (boş ad olursa `saveFolderFor("")` = save kökü).
  Upstream PR #16 bunu düzeltiyor, henüz alınmadı.
- **ANSI dosya API'leri:** `SaveXfer` `CreateFileA`/`MoveFileExA` kullanıyor. Kullanıcı yolunda ASCII dışı karakter olunca
  `STAGING-FAILED` oluşuyor (issue #55, Çince kullanıcı adı). Türkçe kullanıcı adları (ç, ğ, ı, ö, ş, ü) da risk altında.
- Tek makinede iki istemci varken ikisi de aynı `%LOCALAPPDATA%\kenshi\save` klasörünü okur ve host o sırada oraya yazar.
  "Join neyi yükledi" sorusu yarışa bağlıdır (REPLICATION_PITFALLS §13). Join'in yüklediği byte'ları doğrulamadan sonuç çıkarma.
- Engine nesne ömrünü yönetir: tick'ler arasında çıplak `RootObject*` saklama, `hand` sakla ve kullanım anında çöz (§15).
- Tek engine okumasından sonuç çıkarma. Bütçeyi tick ile değil milisaniye ile ifade et (§1–§2).
  Birden fazla yazarı olan değer (para gibi) için state değil operasyon replike et (§16).
- `docs/REPLICATION_PITFALLS.md` dosyasını replikasyon koduna dokunmadan önce oku.
