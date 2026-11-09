# Cemu Android: Grundlage und Performance

Stand: 2026-11-09. Testfall: LEGO City Undercover auf dem AYN Thor Pro.

## Absicht

Cemu auf Android ist auf starken Geräten heute nur unzuverlässig brauchbar: Grafikfehler und ein Spiel, das nicht flüssig läuft. Ziel ist, dass Wii-U-Spiele auf einem Gerät wie dem Thor Pro „einfach spielbar“ sind. LEGO City Undercover (LCU) ist der Testfall, weil der Nutzer es spielen will und es die typischen Probleme zeigt. Verbessert werden soll Cemu auf Android insgesamt, nicht nur dieses eine Spiel.

Dieses Dokument deckt die ersten zwei von vier vereinbarten Projekten ab:

1. **Grundlage**: bauen, auf den Thor bringen, Cemu-Kern aktualisieren
2. **Performance und Grafik**, mit LCU als Testfall
3. später: Dual-Screen und TV per Kabel (GamePad-Bildschirm wählbar)
4. später: TV kabellos

Projekt 3 und 4 bekommen eigene Designs.

## Ziele

- LCU läuft auf dem Thor Pro mit **stabilen 60 FPS**. Das Spiel läuft nativ mit 30 FPS, also braucht es einen 60-FPS-Mod (siehe unten).
- **Kein Flackern, keine Grafikfehler.** Als Referenz gilt, wie das Spiel auf echter Hardware aussieht.
- Ein **60-FPS-Mod ohne Zeitlupe**: Fällt die Bildrate unter 60, darf das Spiel nicht langsamer laufen. Der vorhandene Mod aus `cemu_graphic_packs` hat genau dieses Problem und passt außerdem nur auf v16.

### Messbare Definition (meine Festlegung, vom Nutzer anpassbar)

- „Stabile 60 FPS“: In jeder Testszene liegen mindestens 99 % der Bildzeiten bei höchstens 16,7 ms. Gemessen wird mit warmem Shader-Cache über mindestens 30 Minuten durchgehendes Spielen, damit Überhitzung und Drosselung mit drin sind.
- Shader-Kompilierung: Bei neuen Shadern darf das Spiel nicht einfrieren. Dass ein Objekt beim ersten Auftauchen kurz fehlt (async compile), ist akzeptabel.
- Endabnahme: Der Nutzer spielt auf dem Thor und bestätigt „einfach spielbar“.

## Nicht-Ziele und Anti-Ziele

- **Keine spielspezifischen Hacks im Cemu-Kern.** Braucht LCU Sonderbehandlung, gehört sie in das Spielprofil (`gameProfiles`) oder in ein Graphic Pack.
- **Keine Regressionstests mit anderen Spielen möglich**: Der Nutzer hat nur LCU. Kernänderungen, die das Verhalten anderer Spiele verändern können, werden deshalb auf LCU begrenzt: über eine Option im Spielprofil oder eine Einstellung, standardmäßig aus. Ausgenommen sind eindeutige Fehlerkorrekturen, die sich gegen das Hardware-Verhalten der Wii U oder das Verhalten des Desktop-Cemu begründen lassen.
- **Keine global erzwungenen Genauigkeits-Abkürzungen**, die andere Spiele kaputt machen. Der Fork renenero95-mod schaltet zum Beispiel `GX2DrawdoneSync` global ab. Solche Ideen sind als Spuren nützlich, aber nicht als Lösung.
- Kein Upstream-PR in diesem Projekt. Die Arbeit bleibt im Fork des Nutzers. Der Code soll trotzdem so sauber sein, dass er später einreichbar wäre.
- Projekt 3 und 4 (Bildschirme, TV) gehören nicht hierher.
- Kein Neuschreiben großer Teile (z. B. eines neuen Renderers). Gezielte Verbesserungen, getrieben von Messungen.

## Rahmenbedingungen

**Gerät**: AYN Thor Pro, Snapdragon 8 Gen 2 (meldet sich als QCS8550), Adreno 740, 12 GB RAM (11,5 GB nutzbar), UFS 4.0, Android 13 (API 33), Firmware `Thor_V1.0.0.377_20260206`. Der Original-Vulkan-Treiber ist `V@0676.53` vom 27.12.2023. Oberer Bildschirm 1080×1920 bei 60/120 Hz, unterer 1080×1240.

**Spiel**: LCU Europa, Titel-ID `0005000010101B00`, **Version v0, ohne Update**. Ein Update bekommt der Nutzer nicht: Er hat keine Wii U mehr. Die Datei liegt auf dem Thor unter `/sdcard/emulation/ROMs/wiiu/LEGO City Undercover (Europe) (En,Fr,De,Es,It,Nl,Pt,Da,Ru) (Rev 1).wua` und auf dem Mac unter `/Users/acidic/Documents/git/CEMU-gamedata/`, außerhalb des Repos. Dort liegt auch die schon extrahierte RPX (`lcu-eu-v0/code/legocity.master.rpx`, SHA-256 `0fc4051a…`). Im `.wua` heißt der Titelordner fälschlich `0000000000000000_v0`. Wird der Ordner über die Spielordner-Suche eingebunden, liest Cemu die richtige ID aus `app.xml` (`TitleInfo(path, subPath)` → `ParseXmlInfo`). Nur das direkte Öffnen der Datei scheitert vermutlich (`ParseWuaTitleFolderName` verlangt einen Basistitel). Die Spieldaten gehören dem Nutzer und werden **nirgendwo hochgeladen oder committet**, auch keine Auszüge wie die RPX.
- Für die Analyse darf die RPX auf den Mac gezogen werden, aber nur in einen Ordner außerhalb jedes Commits.
- Disassemblierte Ausschnitte dürfen in der Arbeitsumgebung vorkommen.
- Veröffentlicht werden nur Patch-Adressen, Befehle und `moduleMatches`.

**Treiber**: Auf dem Thor ist Mr. Purple Turnip T30 (Mesa 26.3.0) installiert und in der App aktiv. Treiberdateien stellt der Nutzer bereit. Der Builder lädt keine Treiber-Binaries aus dem Netz.

**Aktuell installiert**: Cemu 0.5.2 vom SapphireRhodonite-Fork (`info.cemu.cemu`, über Obtainium). Einstellungen: Async Compile an, Accurate Barriers an, GX2DrawdoneSync an, VSync aus. Noch kein Shader-Cache, LCU wurde in dieser Installation noch nie gestartet.

**Was 0.5.2 ist**: Gebaut aus SapphireRhodonites Zweig `android-port-dual`, also SSimcos `android-port` plus 5 Commits:
- externer Bildschirm und Bildschirmtausch
- eigene Datenablage mit Spielstand-Synchronisierung
- Graphic-Pack-Ordner
- Versionsnummer

Performance-Änderungen sind keine dabei, und der Kern in `dev` ist neuer.
- 0.5.2 dient nur als **Vergleichspunkt**. Ist unsere Version irgendwo schlechter, ist die Ursachensuche Teil dieses Projekts.
- Die Dual-Screen-Commits sind Vorlage für Projekt 3, nicht für dieses.

**Repository**: Fork `github.com/thisisAcidic/Cemu`, Arbeitszweig `dev`. Er basiert auf SSimcos Zweig `android`, der als Upstream-PR cemu-project/Cemu#1909 eingereicht ist. Remotes: `origin` (Fork), `upstream` (SSimco), `cemu` (offiziell). `dev` hängt `cemu/main` um 146 Commits hinterher, darunter Latte- und Vulkan-Korrekturen vom September 2026. Die Android-App kann eigene Treiber bereits über libadrenotools laden.

**Werkzeuge auf dem Mac**: Android SDK unter `~/Library/Android/sdk`, JDK 17, adb. **Es fehlen** NDK `29.0.14206865` (installiert sind nur 27 und 28) und CMake ≥ 3.25 (im SDK liegt nur 3.22.1). Den Thor erreicht man per Wireless-ADB. Der Port ändert sich nach jedem Neustart, dann muss der Nutzer die neue Adresse nennen.

**Rollen**: Der Nutzer testet auf dem Thor und entscheidet. Claude schreibt den Code.

**Stil (verbindlich)**:
- Commits laufen unter der Git-Identität des Nutzers, **ohne `Co-Authored-By` oder andere KI-Hinweise**, in Commits wie in PRs.
- Der Code folgt `CODING_STYLE.md` und dem Kotlin-Styleguide des Repos. Kommentare so sparsam wie im umgebenden Code.
- Commit-Nachrichten im Repo-Stil `Bereich: Zusammenfassung`.
- Keine typischen KI-Muster wie erklärende Kommentare zu offensichtlichem Code, übertriebene Absicherungen oder aufgeblähte Commit-Texte.

## Vom Nutzer entschieden

- Reihenfolge: Grundlage → Performance → Dual-Screen/TV per Kabel → kabellos
- Ziel 60 FPS statt 30, kein Flackern, keine Grafikfehler
- 60-FPS-Mod ohne Zeitlupe ist gewünscht
- Turnip (Mr. Purple) soll gegen den Original-Treiber getestet werden
- Arbeit nur im eigenen Fork. Keine KI-Namensnennung, keine KI-Muster im Code.
- Git-Ablauf: `dev` als Hauptzweig, ein Zweig pro Thema, alles auf GitHub
- Eigene App-ID neben der bestehenden Installation. Optimierter Build für Messungen.
- Später automatische APK-Releases im Fork für Obtainium

## Ablauf

### Phase 0: Grundlage

1. NDK 29 und CMake ≥ 3.25 über den SDK-Manager installieren. Erster vcpkg-Build für `arm64-android`, der dauert lange.
2. Eine eigene Build-Variante für Tests einrichten:
   - optimierter nativer Code mit Symbolen
   - eigene App-ID neben `info.cemu.cemu` (Name nach Wahl des Builders)
   - Profiling möglich (`profileable`), damit simpleperf und Perfetto funktionieren
   - eigener App-Name, damit beide Installationen auf dem Thor unterscheidbar sind
3. `cemu/main` nach `dev` holen. Merge statt Rebase, weil `dev` schon veröffentlicht ist. Konflikte entstehen vor allem dort, wo der Android-Zweig den Kern angepasst hat (Vulkan-Surface, Dateisystem, Eingabe).
4. Auf dem Thor installieren und einrichten:
   - Spielordner `emulation/ROMs/wiiu` eintragen
   - Turnip T30 und die bisherigen Einstellungen übernehmen. Die Dateien der neuen App-ID liegen unter `/sdcard/Android/data/<id>/files` und sind per adb beschreibbar.
5. Prüfen, dass LCU startet und spielbar läuft. Danach Vergleich mit 0.5.2.
6. Am Ende von Phase 0 oder parallel: ein GitHub-Actions-Release im Fork, das eine signierte APK veröffentlicht, damit Obtainium sie findet.
   - Die Workflow-Struktur gibt es schon (`build.yml`, `deploy_release.yml`).
   - Die Signierschlüssel als Repository-Secrets hinterlegt der Nutzer selbst.

### Phase 1: Messen

1. Mit dem Nutzer 3 bis 5 **wiederholbare Testszenen** festlegen, zum Beispiel:
   - Fahrt durch die Stadt (Streaming, CPU)
   - eine belebte Stelle
   - eine Zwischensequenz
   - eine Szene mit GamePad-Karte

   Cemu hat keine Savestates. Die Szenen werden über Spielstände und kurze Wegbeschreibungen erreicht.
   Die Spielzeit des Nutzers ist knapp, deshalb (meine Festlegung):
   - Vergleichsläufe sind kurz, 2 bis 3 Minuten pro Szene.
   - Läufe, für die niemand spielen muss, macht der Thor allein: das Spiel steht in einer Szene, gemessen werden GPU-Last, Drosselung und Temperatur.
   - Die 30-Minuten-Regel gilt nur für die Endabnahme der gewählten Konfiguration.
   - Nicht jede Kombination aus Treiber, CPU-Modus und Bildrate wird in jeder Szene gemessen. Der Builder grenzt die Kombinationen schrittweise ein.
2. Bildzeiten aufzeichnen, nicht nur die FPS-Anzeige. Dazu kommen: Anteil Shader-Kompilierung, CPU-Last pro Emulator-Thread, GPU-Auslastung, Taktraten und Temperatur über die Zeit.
3. Vergleiche:
   - Original-Treiber gegen Turnip T30
   - die CPU-Modi Single-Core- und Multi-Core-Recompiler. Sie lassen sich im Spielprofil einstellen, das Standardprofil für LCU legt nichts fest.
   - 30 FPS nativ gegen 60 FPS mit portiertem Mod (Phase 2)
4. Prüfen, welche Grafikfehler auftreten, ob sie vom Treiber abhängen und ob sie auch im Desktop-Cemu auftreten. Laut Cemu-Wiki flackert LCU bis 2.6 auf allen Grafikkarten, und die Gesichter der Figuren haben falsche Farben. Als Desktop-Vergleich kann die macOS-Version von Cemu auf dem Mac des Nutzers dienen, wenn das hilft (meine Idee, kein Muss).
5. **Haltepunkt**: Bericht an den Nutzer mit Messwerten, Ursachen und einer nach Wirkung sortierten Liste von Maßnahmen. Der Nutzer entscheidet, was angegangen wird.

### Phase 2: 60-FPS-Mod für v0 (statisch)

Der vorhandene Mod `Lego City Undercover/60FPS` (Version 7) ändert für EU v16 genau einen Befehl:

```
[LCU60FPSEUv16]
moduleMatches = 0xd20260c6
0x025d0d9c = addi r11, r12, 1
```

Die v16-RPX liegt nicht vor, ein Mustervergleich gegen v16 ist also nicht möglich. Die Stelle muss in v0 selbst gefunden werden, zum Beispiel:
- über den Aufruf von `GX2SetSwapInterval`
- über die Bildratenbegrenzung der Engine
- zur Laufzeit mit Cemus Debugger bzw. Logging

Der v16-Patch verrät nur die Art der Änderung: Ein Intervall- oder Zählerwert wird auf 1 gesetzt. Daraus wird eine v0-Variante mit eigenem `moduleMatches`.

Das ist der schnelle Test, ob 60 FPS in Reichweite sind, und liefert die Last für die Messungen aus Phase 1.

### Phase 3: Gezielte Verbesserungen

Was gemacht wird, entscheidet der Nutzer am Haltepunkt nach Phase 1. Erwartbare Felder:
- das Flackern und die Farbfehler im Latte-Renderer
- Shader- bzw. Pipeline-Ruckler
- Leistung des AArch64-Recompilers
- GPU-Last auf Adreno

Jede Verbesserung wird in einem eigenen Themenzweig gebaut, mit Vorher/Nachher-Messung in den Testszenen, und kommt dann nach `dev`.

### Phase 4: 60 FPS ohne Zeitlupe

Den Spielcode untersuchen: Wie leitet die Engine (TT Games) ihren Zeitschritt ab? Das Ziel ist ein Patch, der dem Spiel die tatsächlich vergangene Zeit pro Bild übergibt statt fester 1/60 s, ähnlich wie FPS++ für Breath of the Wild. Physik, Animationen, Zwischensequenzen und Audio müssen dabei stimmen.

Das ist ein Versuch mit offenem Ausgang. Ist es nicht sauber machbar, geht das ehrlich und mit Begründung an den Nutzer zurück. Phase 4 kann parallel zu Phase 3 laufen.

## Meine Entscheidungen (zur Prüfung durch den Nutzer)

- **Planungsdokumente** liegen im verwaisten Zweig `planning`, ausgecheckt als `.planning/` im Repo und lokal per `.git/info/exclude` ignoriert. So landen sie auf GitHub, aber nie in Code-Zweigen, aus denen später PRs werden könnten.
- **Graphic Packs** entstehen in einem Fork von `cemu-project/cemu_graphic_packs` beim Nutzer, Ordner `src/LEGOCityUndercover/...`, damit sie später dort einreichbar wären. Auf dem Thor kommen sie per adb in den Graphic-Pack-Ordner der Test-App.
- Die messbare 60-FPS-Definition oben.
- Merge statt Rebase für `cemu/main` → `dev`. Neue Stände von `upstream/android` werden bei Bedarf ebenfalls nach `dev` gemergt.
- **Zwei eigene App-IDs**, beide neben `info.cemu.cemu`:
  - Test-Variante: lokal gebaut, per adb installiert, mit dem Debug-Schlüssel signiert.
  - Release für Obtainium: mit dem Schlüssel des Nutzers signiert.

  Getrennt, weil Android unterschiedlich signierte APKs nicht übereinander installiert. Sonst gingen bei jedem Wechsel die App-Daten verloren. Erst wenn unsere Version 0.5.2 in allem schlägt (inklusive Dual-Screen aus Projekt 3), kann der Nutzer 0.5.2 deinstallieren.
- Umgang mit der RPX wie unter „Spiel“ beschrieben: lokal analysieren, nur Patch-Daten veröffentlichen.
- Messumfang wie in Phase 1 beschrieben: kurze Vergleichsläufe, unbeaufsichtigte Läufe, 30 Minuten nur zur Abnahme.

## Dem Builder überlassen

- Name der App-ID und der Build-Variante, genaue Compiler-Flags
- Werkzeuge zum Messen (simpleperf, Perfetto, Android GPU Inspector, eigenes Bildzeit-Logging in Cemu) und wie Messwerte abgelegt werden
- Konkrete Testszenen, gemeinsam mit dem Nutzer
- Technischer Ansatz jeder Verbesserung in Phase 3
- Analysewerkzeuge für den Spielcode in Phase 2 und 4

## Risiken

- 60 FPS könnten an der CPU-Emulation scheitern. Der AArch64-Recompiler ist erst seit Juli 2025 in Cemu und weniger ausgereift als der x86-Recompiler. Die Messungen in Phase 1 zeigen das früh.
- Der Thor drosselt bei längerer Last. Kurze Messungen allein können also täuschen.
- Original-Treiber und Turnip haben jeweils eigene Fehler. Gut möglich, dass am Ende ein Treiber klar empfohlen wird.
- v0 ohne Update: Falls das Update spielseitige Fehler behoben hat, müssen sie per Patch nachgezogen werden.
- Der Zeitschritt-Patch kann an der Engine scheitern.
