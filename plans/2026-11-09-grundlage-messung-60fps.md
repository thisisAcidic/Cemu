# Grundlage, Messung und 60-FPS-Patch für v0: Implementierungsplan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eine messbare, auf dem Thor laufende Test-Version von Cemu mit aktuellem Kern, ein 60-FPS-Patch für LCU v0 und eine erste Messreihe, die am Haltepunkt zeigt, wo die Zeit verloren geht.

**Architecture:** Eigene Build-Variante `profile` mit eigener App-ID neben der installierten 0.5.2. `cemu/main` wird in `dev` gemergt. Ein abschaltbarer Bildzeit-Logger im Latte-Performance-Monitor schreibt CSV-Dateien, die ein Python-Skript im Planungszweig auswertet. Der 60-FPS-Patch wird im Fork von `cemu_graphic_packs` als zusätzliche Patch-Gruppe des bestehenden Packs gebaut.

**Tech Stack:** C++20/CMake/vcpkg (NDK 29), Kotlin/Gradle (AGP 9.2.1, Gradle 9.4.1), Vulkan, Cemu-Graphic-Pack-Format, Python 3 (stdlib, plus `capstone` in einem eigenen venv nur für die RPX-Analyse), adb, simpleperf.

**Spec:** `.planning/specs/2026-11-09-grundlage-performance-design.md`. Dieser Plan deckt Phase 0, 1 und 2 ab und endet am Haltepunkt. Phase 3 und 4 bekommen eigene Pläne, sobald der Nutzer am Haltepunkt entschieden hat.

## Global Constraints

- Commits unter der Git-Identität des Nutzers (`thisisAcidic <supergamer4092@gmail.com>`), **ohne `Co-Authored-By` und ohne jeden KI-Hinweis**, auch nicht in PR-Texten.
- Code folgt `CODING_STYLE.md` und `src/android/` Kotlin-Stil. Kommentare so sparsam wie der umgebende Code. Keine erklärenden Kommentare zu offensichtlichem Code, keine übertriebenen Prüfungen und kein überflüssiges Logging.
- Commit-Nachrichten im Repo-Stil `Bereich: kurze Zusammenfassung` (Beispiele aus `dev`: `android: add android app`, `Vulkan: add support for custom drivers on android`).
- Spieldaten (RPX, `.wua`, Disassembly-Dumps) liegen nur unter `/Users/acidic/Documents/git/CEMU-gamedata/` und werden **nie committet oder hochgeladen**. Veröffentlicht werden nur Adressen, Befehle und `moduleMatches`.
- NDK `29.0.14206865`, CMake `3.31.6` aus dem SDK (Projekt verlangt `3.25.0+`), `minSdk = 30`, nur `arm64-v8a`.
- Die Test-App `info.cemu.cemu.profile` darf `info.cemu.cemu` (0.5.2) und dessen Daten nicht berühren.
- Treiber-Binaries werden nicht aus dem Netz geladen. Turnip T30 liegt schon auf dem Thor unter `/sdcard/Android/data/info.cemu.cemu/files/customDrivers/fe1e2b85-a742-46d9-8884-23abd7ea946f/`.
- Jedes Thema in einem eigenen Zweig von `dev`, Merge nach `dev` (kein Rebase veröffentlichter Zweige), Push nach `origin`.
- Thor per Wireless-ADB: `ANDROID_SERIAL` auf die aktuelle Adresse setzen (zuletzt `172.27.23.170:35093`). Nach einem Neustart des Thor die neue Adresse beim Nutzer erfragen.
- Ohne Regressionsspiele: Kernänderungen, die das Verhalten anderer Spiele ändern können, nur über Spielprofil oder Einstellung, standardmäßig aus.

## Review Focus

1. **Parallelinstallation**: Nach Installation und Einrichtung der Test-App startet 0.5.2 weiter mit Turnip und unveränderten Einstellungen. Test in Task 2, Schritt 5.
2. **Logger aus = kein Effekt, Logger an = Daten auch nach Force-Stop**: Ohne Marker-Datei wird keine Datei angelegt. Mit Marker überlebt die CSV ein `am force-stop` bis auf höchstens die letzten 120 Bilder. Test in Task 4, Schritte 6 und 7.
3. **Patch nur für v0**: Die neue Patch-Gruppe greift nur bei der v0-Prüfsumme. Die bestehende v16-Gruppe bleibt Byte für Byte unverändert. Test in Task 5, Schritte 6 und 7.
4. **Spieltempo mit 60-FPS-Patch**: Bei erreichten 60 FPS läuft eine feste Sequenz gleich lang wie bei 30 FPS ohne Patch (±3 %). Doppelt so schnell oder halb so schnell wäre ein stiller Fehler. Test in Task 5, Schritt 8.
5. **Merge bricht nichts Android-Spezifisches**: Nach dem Merge lädt die App weiter den eigenen Treiber, und Async-Shader-Compile ist aktiv. Prüfung über `log.txt` in Task 3, Schritt 6.

---

### Task 1: Werkzeuge und Profil-Build-Variante

**Files:**
- Modify: `src/android/app/build.gradle.kts` (Block `buildTypes`, ca. Zeile 86–102)
- Create: `src/android/app/src/profile/AndroidManifest.xml`
- Create: `src/android/app/src/profile/res/values/strings.xml`

**Interfaces:**
- Produces: Build-Typ `profile`, Gradle-Task `:app:assembleProfile`, APK `src/android/app/build/outputs/apk/profile/app-profile.apk`, Paketname `info.cemu.cemu.profile`, App-Name `Cemu profile`, unstripped Libraries unter `src/android/app/build/intermediates/merged_native_libs/profile/` (für simpleperf `--symfs`).

- [ ] **Step 1: SDK-Pakete installieren**

Run: `~/Library/Android/sdk/cmdline-tools/latest/bin/sdkmanager "ndk;29.0.14206865" "cmake;3.31.6"`
Expected: `ls ~/Library/Android/sdk/ndk/29.0.14206865/source.properties` existiert, `~/Library/Android/sdk/cmake/3.31.6/bin/cmake --version` meldet `3.31.6`.

- [ ] **Step 2: Branch anlegen**

```bash
git -C /Users/acidic/Documents/git/CEMU switch -c build/profile-variant dev
```

- [ ] **Step 3: Build-Typ `profile` in `build.gradle.kts`**

`create("profile")` mit `initWith(getByName("release"))`, `applicationIdSuffix = ".profile"`, `isMinifyEnabled = false`, `signingConfig = signingConfigs.getByName("debug")`, `matchingFallbacks += listOf("release")`. Nativ mit Optimierung und Symbolen: `externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=RelWithDebInfo" } }` im Build-Typ.

- [ ] **Step 4: Manifest und Name für `profile`**

`src/profile/AndroidManifest.xml` ergänzt in `<application>` nur `<profileable android:shell="true" />`. `src/profile/res/values/strings.xml` wie `src/debug/res/values/strings.xml`, aber mit `Cemu profile`.

- [ ] **Step 5: Bauen**

Run: `cd /Users/acidic/Documents/git/CEMU/src/android && ./gradlew :app:assembleProfile` (im Hintergrund, der erste vcpkg-Lauf dauert lange)
Expected: `BUILD SUCCESSFUL`. Scheitert der Build an der Java-Version (CI nutzt JDK 21, lokal ist JDK 17), Temurin 21 per `brew install --cask temurin@21` nach Rückfrage beim Nutzer installieren und `JAVA_HOME` darauf setzen.

- [ ] **Step 6: APK prüfen**

Run: `~/Library/Android/sdk/build-tools/36.0.0/aapt2 dump badging src/android/app/build/outputs/apk/profile/app-profile.apk | grep -E "package:|application-label:"`
Expected: `package: name='info.cemu.cemu.profile'` und `application-label:'Cemu profile'`. Außerdem stammt `-O2` oder `-O3` zusammen mit `-g` aus `src/android/app/.cxx/RelWithDebInfo/*/arm64-v8a/compile_commands.json` (`grep -c -- "-O2 -g\|-O3 -g\|-g -O2\|-g -O3"` > 0).

- [ ] **Step 7: Commit und Push**

```bash
git add src/android/app/build.gradle.kts src/android/app/src/profile
git commit -m "android: add profile build type"
git push -u origin build/profile-variant
```

---

### Task 2: Test-App auf dem Thor einrichten, Basislauf, v0-Prüfsumme

**Interfaces:**
- Consumes: `app-profile.apk` aus Task 1.
- Produces: eingerichtete Test-App auf dem Thor, Datenordner `/sdcard/Android/data/info.cemu.cemu.profile/files/`, die v0-Prüfsumme `0x????????` aus der Zeile `Loaded module 'legocity.master' with checksum 0x…` in `log.txt`. Sie wird in `.planning/reports/fakten.md` notiert, denn Task 5 braucht sie.

- [ ] **Step 1: Installieren**

Run: `adb install -r src/android/app/build/outputs/apk/profile/app-profile.apk && adb shell pm list packages | grep cemu`
Expected: `info.cemu.cemu` und `info.cemu.cemu.profile` sind beide gelistet.

- [ ] **Step 2: Nutzer startet die App einmal und fügt den Spielordner hinzu**

Der Nutzer öffnet „Cemu profile“ und wählt über die Ordnerauswahl `emulation/ROMs/wiiu` als Spielordner. Die Auswahl läuft über das Storage Access Framework und lässt sich nicht per adb erledigen. LCU erscheint in der Liste. Danach `adb shell am force-stop info.cemu.cemu.profile`.

- [ ] **Step 3: Treiber und Einstellungen übernehmen**

`customDrivers/fe1e2b85-…` von `info.cemu.cemu` nach `info.cemu.cemu.profile/files/customDrivers/` kopieren (`adb shell cp -r`). In der neuen `settings.xml` setzen: `custom_driver_path` auf den neuen Pfad, `AsyncCompile=true`, `vkAccurateBarriers=true`, `GX2DrawdoneSync=true`, `VSync=0`, Overlay `FPS=true`. `GamePaths` bleibt, wie die App es in Step 2 gespeichert hat.

- [ ] **Step 4: Basislauf**

Der Nutzer startet LCU und spielt 2 bis 3 Minuten. Danach: `adb pull /sdcard/Android/data/info.cemu.cemu.profile/files/log.txt /Users/acidic/Documents/git/CEMU-gamedata/logs/task2-log.txt`
Expected: Das Log enthält `Loaded module 'legocity.master' with checksum 0x` und den Namen des Turnip-Treibers (Mesa/Turnip) in der Vulkan-Geräteinfo. Prüfsumme, Treiberzeile und Eindruck des Nutzers (FPS, Fehler) kommen in `.planning/reports/fakten.md`.

- [ ] **Step 5: 0.5.2 ist unberührt (Review Focus 1)**

Der Nutzer startet die alte „Cemu“-App einmal. Run: `adb shell cat /sdcard/Android/data/info.cemu.cemu/files/settings.xml | grep custom_driver_path`
Expected: weiter der alte Pfad unter `info.cemu.cemu`, und die App startet normal.

- [ ] **Step 6: Commit (Planungszweig)**

```bash
git -C /Users/acidic/Documents/git/CEMU/.planning add reports/fakten.md
git -C /Users/acidic/Documents/git/CEMU/.planning commit -m "Record baseline facts for LCU v0 on Thor"
git -C /Users/acidic/Documents/git/CEMU/.planning push
```

---

### Task 3: `cemu/main` nach `dev` mergen

**Files (Konflikte laut `git merge-tree`):**
- `src/CMakeLists.txt` (um Zeile 73)
- `src/util/CMakeLists.txt` (um Zeile 40)
- `src/Cafe/OS/libs/swkbd/swkbd.cpp` (vier Stellen ab Zeile 94)

**Interfaces:**
- Consumes: `build/profile-variant` (Task 1, wird vorher nach `dev` gemergt).
- Produces: `dev` mit Stand `cemu/main` vom 2026-11-09, Android-Build grün.

- [ ] **Step 1: Task 1 nach `dev` holen und Merge-Branch anlegen**

```bash
git -C /Users/acidic/Documents/git/CEMU switch dev && git -C /Users/acidic/Documents/git/CEMU merge --no-ff build/profile-variant
git -C /Users/acidic/Documents/git/CEMU fetch cemu && git -C /Users/acidic/Documents/git/CEMU switch -c merge/cemu-main-2026-11
git -C /Users/acidic/Documents/git/CEMU merge cemu/main
```
Expected: Konflikte genau in den drei oben genannten Dateien.

- [ ] **Step 2: Konflikte auflösen**

- `src/CMakeLists.txt`: Die Android-Verzweigung bleibt (`if(ANDROID) add_library(CemuBin STATIC …)`, sonst `add_executable(CemuBin …)`). Upstreams neues `cemu_use_precompiled_header(CemuBin)` gilt für beide Zweige. Bricht es den Android-Build, gilt es nur außerhalb von Android.
- `src/util/CMakeLists.txt`: Beide Einträge behalten (`helpers/ZArchiveHelpers.h`, `helpers/ZipArchive.h`), sofern beide Dateien im Merge-Stand existieren.
- `swkbd.cpp`: Upstreams neue typisierte Funktionen mit `return 1;` übernehmen. Die Aufrufe von `g_swkbdCallbacks` aus `dev` kommen jeweils vor das `return` in `SwkbdAppearInputForm`, `SwkbdAppearKeyboard` (mit der `maxLength`-Berechnung) und `SwkbdDisappearInputForm`. `g_swkbdCallbacks` und `swkbd::setSwkbdCallbacks` bleiben. Alle anderen Callback-Aufrufe der `dev`-Version ebenso übertragen (`grep -n g_swkbdCallbacks` vorher und nachher, gleiche Anzahl Aufrufer).

- [ ] **Step 3: Bauen**

Run: `cd src/android && ./gradlew :app:assembleProfile`
Expected: `BUILD SUCCESSFUL`. Weitere Fehler ohne Textkonflikt (geänderte Signaturen, die der Android-Code nutzt) werden im selben Merge-Commit behoben.

- [ ] **Step 4: Merge-Commit**

```bash
git add -A && git commit --no-edit
```
Nachricht bleibt die Standardnachricht `Merge remote-tracking branch 'cemu/main' into merge/cemu-main-2026-11`.

- [ ] **Step 5: Auf dem Thor installieren und LCU 2 bis 3 Minuten spielen lassen**

Run: `adb install -r src/android/app/build/outputs/apk/profile/app-profile.apk`

- [ ] **Step 6: Treiber und Async-Compile prüfen (Review Focus 5)**

`log.txt` ziehen wie in Task 2, Step 4. Expected: Turnip steht weiter in der Geräteinfo. Shader-Kompilierung meldet keine Fehler, und das Spiel friert bei neuen Shadern nicht ein (Eindruck des Nutzers).

- [ ] **Step 7: Nach `dev` mergen und pushen**

```bash
git switch dev && git merge --no-ff merge/cemu-main-2026-11 && git push origin dev merge/cemu-main-2026-11
```

---

### Task 4: Bildzeit-Logger und Auswertung

**Files:**
- Create: `src/Cafe/HW/Latte/Core/LatteFrameTimeLog.h`, `src/Cafe/HW/Latte/Core/LatteFrameTimeLog.cpp`
- Modify: `src/Cafe/HW/Latte/Core/LattePerformanceMonitor.cpp` (`LattePerformanceMonitor_frameEnd`, Zeile 7–13)
- Modify: `src/Cafe/CMakeLists.txt` (neue Dateien eintragen, neben `LattePerformanceMonitor.cpp`)
- Create: `.planning/tools/frametimes.py`, `.planning/tools/test_frametimes.py`

**Interfaces:**
- Produces (C++): `void LatteFrameTimeLog_OnFrameEnd(uint64 shaderCreateTimeUs);`
  - Beim ersten Aufruf pro Prozess wird geprüft, ob `ActiveSettings::GetUserDataPath("frametimes.enable")` existiert. Wenn nicht, passiert danach nie wieder etwas.
  - Wenn ja, wird `GetUserDataPath("frametimes.csv")` neu angelegt. Kopfzeile: `frame,timestamp_us,frametime_us,shadercreate_us`. Danach eine Zeile pro Bild, Zeitbasis `HighResolutionTimer`, Schreiben über `FileStream` wie im Rest von Cemu, Flush alle 120 Bilder.
- Produces (Python): `summarize(rows: list[dict]) -> dict` mit den Schlüsseln `frames`, `avg_fps`, `p99_ms`, `pct_within_16_7`, `frames_over_50ms`, `shadercreate_ms_total`. CLI: `python3 frametimes.py <csv> [--from-s N] [--to-s M]` druckt diese Werte.

- [ ] **Step 1: Failing test für `summarize`**

```python
class SummarizeTest(unittest.TestCase):
    def rows(self, frametimes_us, shader_us=0):
        t = 0
        out = []
        for i, ft in enumerate(frametimes_us):
            t += ft
            out.append({'frame': i, 'timestamp_us': t, 'frametime_us': ft, 'shadercreate_us': shader_us})
        return out

    def test_steady_60(self):
        s = summarize(self.rows([16666] * 600))
        self.assertEqual(s['frames'], 600)
        self.assertAlmostEqual(s['avg_fps'], 60.0, places=1)
        self.assertEqual(s['pct_within_16_7'], 100.0)
        self.assertEqual(s['frames_over_50ms'], 0)

    def test_one_hitch(self):
        s = summarize(self.rows([16666] * 99 + [50001]))
        self.assertEqual(s['frames_over_50ms'], 1)
        self.assertEqual(s['pct_within_16_7'], 99.0)
        self.assertGreater(s['p99_ms'], 16.7)

    def test_shader_time_sum(self):
        self.assertAlmostEqual(summarize(self.rows([33333] * 10, shader_us=1500))['shadercreate_ms_total'], 15.0)
```

- [ ] **Step 2: Test laufen lassen**

Run: `python3 -I -m unittest discover -s /Users/acidic/Documents/git/CEMU/.planning/tools -v`
Expected: FAIL (`summarize` nicht definiert).

- [ ] **Step 3: `summarize` und CLI in `frametimes.py`** (nur stdlib, `csv` und `statistics`. `p99_ms` ist das 99. Perzentil der `frametime_us`, nearest-rank.)

- [ ] **Step 4: Test laufen lassen**

Gleicher Befehl. Expected: 3 Tests OK.

- [ ] **Step 5: Logger in C++ und Aufruf**

Branch `perf/frametime-log` von `dev`. Der Aufruf kommt in `LattePerformanceMonitor_frameEnd` direkt nach `gpuTime_shaderCreate.frameFinished()`:
`LatteFrameTimeLog_OnFrameEnd(PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_shaderCreate.getPreviousFrameValue()));`
Danach mit `./gradlew :app:assembleProfile` bauen und installieren.

- [ ] **Step 6: Aus-Zustand prüfen (Review Focus 2)**

Ohne Marker LCU kurz starten. Run: `adb shell ls /sdcard/Android/data/info.cemu.cemu.profile/files/ | grep frametimes`
Expected: keine Ausgabe.

- [ ] **Step 7: An-Zustand und Force-Stop prüfen (Review Focus 2)**

`adb shell touch …/files/frametimes.enable`. Der Nutzer spielt 60 s, danach `adb shell am force-stop info.cemu.cemu.profile`, CSV nach `CEMU-gamedata/logs/` ziehen.
Run: `python3 -I .planning/tools/frametimes.py CEMU-gamedata/logs/<datei>.csv`
Expected: `frames` liegt mindestens bei 60 s × gemessener FPS − 120, `avg_fps` passt zur Overlay-Anzeige (±1), und die Kopfzeile stimmt.

- [ ] **Step 8: Commits und Merge**

```bash
git add src/Cafe/HW/Latte/Core/LatteFrameTimeLog.* src/Cafe/HW/Latte/Core/LattePerformanceMonitor.cpp src/Cafe/CMakeLists.txt
git commit -m "Latte: add optional frame time log"
git switch dev && git merge --no-ff perf/frametime-log && git push origin dev perf/frametime-log
git -C .planning add tools/frametimes.py tools/test_frametimes.py && git -C .planning commit -m "Add frame time summary tool" && git -C .planning push
```

---

### Task 5: 60-FPS-Patch für LCU EU v0

**Files:**
- Fork: `cemu-project/cemu_graphic_packs` → `thisisAcidic/cemu_graphic_packs`, Klon unter `/Users/acidic/Documents/git/cemu_graphic_packs`, Branch `lcu-60fps-v0`
- Modify: `src/LEGOCityUndercover/Mods/60fps/patches.txt` (neue Gruppe anhängen)
- Create: `.planning/tools/rpx.py` (RPX-Sektionen entpacken, `.text` mit Basisadresse ausgeben, Importe auflösen)

**Interfaces:**
- Consumes: v0-Prüfsumme aus `.planning/reports/fakten.md` (Task 2). RPX unter `/Users/acidic/Documents/git/CEMU-gamedata/lcu-eu-v0/code/legocity.master.rpx`.
- Produces: Patch-Gruppe `[LCU60FPSEUv0]` im bestehenden Pack `Lego City Undercover/60FPS`.

- [ ] **Step 1: Analyse-Umgebung**

venv unter `/Users/acidic/Documents/git/CEMU-gamedata/venv` mit `pip install capstone`. `rpx.py` entpackt Sektionen mit dem Flag `SHF_RPL_ZLIB` (`0x08000000`, die ersten 4 Bytes sind die entpackte Größe, danach ein zlib-Strom). Es gibt `.text` mit `sh_addr` als Basis aus und listet die Einträge in `.fimport_gx2` und `.fimport_coreinit` mit ihren Stub-Adressen.
Expected: Die Ausgabe enthält Stubs für `GX2SetSwapInterval` und/oder `GX2WaitForVsync` bzw. `GX2GetSwapStatus`. Dumps landen nur in `CEMU-gamedata/analysis/`.

- [ ] **Step 2: Stelle finden**

Aufrufer dieser Stubs in `.text` suchen und in der Bildraten-Logik nach dem Gegenstück zum v16-Patch suchen (`addi r11, r12, 1` an `0x025d0d9c` in v16, vermutlich vorher `addi …, 2`). Ergebnis: eine v0-Adresse, das Originalwort und eine Begründung in zwei, drei Sätzen. Das kommt in `.planning/reports/fakten.md`.

- [ ] **Step 3: Fork und Branch**

```bash
gh repo fork cemu-project/cemu_graphic_packs --clone=false
git clone git@github.com:thisisAcidic/cemu_graphic_packs.git /Users/acidic/Documents/git/cemu_graphic_packs
git -C /Users/acidic/Documents/git/cemu_graphic_packs remote add cemu git@github.com:cemu-project/cemu_graphic_packs.git
git -C /Users/acidic/Documents/git/cemu_graphic_packs fetch cemu
git -C /Users/acidic/Documents/git/cemu_graphic_packs switch -c lcu-60fps-v0 cemu/master
```

- [ ] **Step 4: Patch-Gruppe anhängen**

```
[LCU60FPSEUv0]
moduleMatches = 0x<v0-Prüfsumme>
0x<v0-Adresse> = <Befehl wie v16, Register nach v0-Code>
```
`rules.txt` bleibt unverändert (`titleIds` deckt `0005000010101B00` schon ab, `version = 7` ist die Formatversion).

- [ ] **Step 5: Auf den Thor bringen**

Den Ordner `src/LEGOCityUndercover/Mods/60fps` nach `/sdcard/Android/data/info.cemu.cemu.profile/files/graphicPacks/LEGOCityUndercover_60fps/` pushen. Ist dort auch eine heruntergeladene Fassung desselben Packs (`downloadedGraphicPacks`), wird sie für den Test entfernt. Der Nutzer aktiviert „60FPS“ für LCU in der App.

- [ ] **Step 6: Patch greift bei v0 (Review Focus 3)**

LCU starten, `log.txt` ziehen. Expected: `Applying patch group 'LCU60FPSEUv0'`, kein `Patches: Address … is not within`, und `LCU60FPSEUv16` wird nicht angewendet.

- [ ] **Step 7: v16-Gruppe unverändert (Review Focus 3)**

Run: `git -C /Users/acidic/Documents/git/cemu_graphic_packs diff cemu/master -- src/LEGOCityUndercover/Mods/60fps/patches.txt`
Expected: nur hinzugefügte Zeilen.

- [ ] **Step 8: Tempo prüfen (Review Focus 4)**

Mit dem Nutzer eine feste Sequenz wählen, die mit Patch 60 FPS erreicht (Menü-Animation, Zwischensequenz oder Ladeübergang). Ihre Dauer messen, mit Patch und ohne (30 FPS), über Bildzeit-CSV-Zeitstempel oder Stoppuhr.
Expected: gleiche Dauer ±3 %, und die FPS-Anzeige zeigt ~60 bzw. ~30.

- [ ] **Step 9: Commit und Push**

```bash
git -C /Users/acidic/Documents/git/cemu_graphic_packs add src/LEGOCityUndercover/Mods/60fps/patches.txt
git -C /Users/acidic/Documents/git/cemu_graphic_packs commit -m "[Lego City Undercover] Add 60fps patch for EU v0"
git -C /Users/acidic/Documents/git/cemu_graphic_packs push -u origin lcu-60fps-v0
git -C /Users/acidic/Documents/git/CEMU/.planning add tools/rpx.py reports/fakten.md && git -C /Users/acidic/Documents/git/CEMU/.planning commit -m "Add RPX analysis tool and v0 frame limiter notes" && git -C /Users/acidic/Documents/git/CEMU/.planning push
```

---

### Task 6: Messreihe Phase 1 und Haltepunkt

**Files:**
- Create: `.planning/reports/testszenen.md`, `.planning/tools/thermal_log.sh`, `.planning/reports/2026-11-messung-runde1.md`

**Interfaces:**
- Consumes: Test-App mit Logger (Task 4), 60-FPS-Patch (Task 5), `frametimes.py`.
- Produces: Bericht mit Tabelle pro Szene und Konfiguration (`avg_fps`, `p99_ms`, `pct_within_16_7`, `frames_over_50ms`, `shadercreate_ms_total`, beobachtete Grafikfehler, Temperatur/Takt). Dazu Ursachen und eine nach Wirkung sortierte Maßnahmenliste für Phase 3.

- [ ] **Step 1: Testszenen**

Mit dem Nutzer 3 bis 5 Szenen festlegen und in `testszenen.md` beschreiben, wie man hinkommt (Spielstand, Weg, was im Bild ist), zum Beispiel Stadtfahrt, belebter Platz, Zwischensequenz, GamePad-Karte.

- [ ] **Step 2: `thermal_log.sh`**

Liest per `adb shell` alle 5 s `/sys/class/thermal/thermal_zone*/temp` (mit `type`), `scaling_cur_freq` aller CPUs, `/sys/class/kgsl/kgsl-3d0/gpuclk` und `gpu_busy_percentage` und schreibt CSV nach `CEMU-gamedata/logs/`. Pfade, die ohne Root nicht lesbar sind, werden einmal gemeldet und übersprungen.
Expected: 1 Minute Probelauf liefert mindestens CPU-Takte und Temperaturen.

- [ ] **Step 3: Runde 1, Treiber**

Original-Treiber (leerer `custom_driver_path`) gegen Turnip T30, 30 FPS ohne Patch, CPU-Modus Multi-Core-Recompiler, Szenen A und B, je 2 bis 3 Minuten. Grafikfehler je Treiber notieren. Als Referenz läuft 0.5.2 mit gleichem Treiber kurz in Szene A, mit FPS-Overlay und dem Eindruck des Nutzers, denn 0.5.2 hat keinen Logger. Ist die Test-App schlechter als 0.5.2, kommt das als eigener Punkt in den Bericht. Ergebnis: Arbeitstreiber für Runde 2 und 3.

- [ ] **Step 4: Runde 2, CPU-Modus**

Arbeitstreiber, Single-Core- gegen Multi-Core-Recompiler (Spielprofil `0005000010101b00.ini`, `[CPU] cpuMode`), Szenen A und B.

- [ ] **Step 5: Runde 3, 60 FPS**

Beste Konfiguration mit 60-FPS-Patch, alle Szenen. Je Szene ein `simpleperf record --app info.cemu.cemu.profile -g --duration 20`, ausgewertet mit `--symfs` auf die Libraries aus Task 1. Ergebnis: Anteil JIT-Code (Spielcode) gegen Emulator-C++ und die größten Emulator-Funktionen.

- [ ] **Step 6: Dauerlauf**

Beste Konfiguration mit Patch, 30 Minuten in Szene A ohne Eingriff, dabei `thermal_log.sh` und Bildzeit-Logger. Ergebnis: Verlauf von FPS, Takt und Temperatur, und ob und wann gedrosselt wird.

- [ ] **Step 7: Optional, Desktop-Vergleich der Grafikfehler**

Nur wenn Fehler auftreten, deren Ursache unklar ist: Cemu für macOS mit der `.wua` unter `CEMU-gamedata/` laufen lassen. Download des offiziellen Releases von `github.com/cemu-project/Cemu/releases` nur nach Rückfrage (Dateiname und Größe nennen), sonst aus dem Repo bauen.

- [ ] **Step 8: Bericht und Haltepunkt**

`2026-11-messung-runde1.md` schreiben und committen und pushen (Planungszweig). Dem Nutzer die Kernaussagen und die Maßnahmenliste vorlegen. **Hier endet der Plan.** Der Nutzer entscheidet, was in Phase 3 kommt.

---

### Task 7: APK-Releases für Obtainium

**Files:**
- Modify: `src/android/app/build.gradle.kts` (Build-Typ `fork` neben `profile`)
- Create: `src/android/app/src/fork/res/values/strings.xml`
- Create: `.github/workflows/android_fork_release.yml`

**Interfaces:**
- Consumes: Muster des Jobs `build-android` in `.github/workflows/build.yml` (Checkout mit Submodulen, `msgunfmt`-Schritt, vcpkg-Binary-Cache-Action, JDK 21, Keystore aus `ANDROID_STORE_FILE_BASE64`). Secrets `ANDROID_STORE_FILE_BASE64`, `ANDROID_KEY_STORE_PASSWORD`, `ANDROID_KEY_ALIAS` legt der Nutzer selbst an.
- Produces: Bei Tag `android-v<major>.<minor>` entsteht ein GitHub-Release mit `cemu-fork-<major>.<minor>.apk`, Paketname `info.cemu.cemu.fork`, App-Name `Cemu Fork`.

- [ ] **Step 1: Build-Typ `fork`**

Wie `profile` in Task 1, aber `initWith(getByName("release"))` mit Minify und Release-Signierung, `applicationIdSuffix = ".fork"`, `matchingFallbacks += listOf("release")`, ohne `profileable`. `src/fork/res/values/strings.xml` setzt `Cemu Fork`. `release` bleibt unverändert.

- [ ] **Step 2: Workflow**

Eigener Workflow nur im Fork, Trigger `push: tags: ['android-v*']`, ein Job nach dem Muster von `build-android`. `EMULATOR_VERSION_MAJOR`/`MINOR` kommen aus dem Tag. Gebaut wird `./gradlew assembleFork`, danach `gh release create "$GITHUB_REF_NAME"` mit der umbenannten APK. `build.yml` und `deploy_release.yml` bleiben unverändert.

- [ ] **Step 3: Nutzer hinterlegt Secrets**

Anleitung für den Nutzer: Keystore mit `keytool` erzeugen und die drei Secrets unter Settings → Secrets → Actions anlegen. Das macht der Nutzer selbst.

- [ ] **Step 4: Probelauf**

Run: `git tag android-v0.1 && git push origin android-v0.1`, danach `gh run watch`.
Expected: Workflow grün, Release `android-v0.1` mit APK. `aapt2 dump badging` zeigt `info.cemu.cemu.fork` und `Cemu Fork`. Der Nutzer fügt `github.com/thisisAcidic/Cemu` in Obtainium hinzu, und die App installiert sich neben 0.5.2 und der Test-App.

- [ ] **Step 5: Commit**

```bash
git add src/android/app/build.gradle.kts src/android/app/src/fork .github/workflows/android_fork_release.yml
git commit -m "android: add fork build type and release workflow"
git push origin dev
```
