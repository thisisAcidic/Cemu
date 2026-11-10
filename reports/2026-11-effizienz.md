# Effizienz: Wo Cemu auf dem Thor Leistung verschenkt

Stand 2026-11-09. Grundlage: simpleperf-Profil vom Hafen (r3, Small Underclock) und neue Messläufe mit Akkuleistung (Stock-Takt, Qualcomm-Treiber, 60-FPS-Patch, Dual Screen aktiv).

## Befunde

1. **Leerlauf wird nicht geschlafen, sondern durchgerechnet.**
   - Emulierter Kern 1: Hat er keinen lauffähigen PPC-Thread, dreht seine Idle-Schleife ununterbrochen. Sie fragt Audio, Alarme und die Run-Queue ab. Rund 14 % seiner Samples entfallen darauf (`clock_gettime` 6,6 %, TSC-Spinlock 2,1 %, Locks).
   - GPU-Thread: Ohne neue Befehle pollt er Ringpuffer, Vsync-Timer und `sched_yield`. Das sind rund 15 % seiner Samples.
   - Beide Kerne laufen deshalb immer mit vollem Takt, auch wenn sie nur warten. Der Governor kann nicht heruntertakten.
2. **Zeitabfrage pro Draw-Call.** Der GPU-Thread liest für jeden Draw die Uhr, nur damit die Textur-Speicherbereinigung (Auflösung 100 ms) einen Zeitstempel hat.
3. **Dual Screen zerstört das Pipelining.** Vor jedem Present wartet der Renderer auf den Befehlspuffer des vorherigen Presents. Mit GamePad-Fenster ist das der TV-Present desselben Bildes. Jedes Bild wartet also, bis die GPU es fertig gerendert hat, und der GPU-Thread kann nicht vorarbeiten.
   - Messung: Hafen mit Dual Screen 55,0 FPS (p99 23,8 ms, nur 18 % der Bilder ≤ 16,7 ms) gegen 59,9 FPS ohne Dual Screen.

## Änderungen

| Branch | Änderung |
|---|---|
| `perf/idle-waits` | Kern 1 schläft nach 50 µs Leerlauf bis zum nächsten Alarm oder lauffähigen Thread (max. 0,5 ms wegen Audio) |
| `perf/idle-waits` | GPU-Thread schläft nach 50 µs ohne Befehle bis zu neuen Befehlen, Async-Arbeit oder dem nächsten Vsync (max. 1 ms) |
| `perf/idle-waits` | Draw-Zeitstempel einmal pro Bild statt pro Draw |
| `perf/idle-waits` | Optional `jitmap.enable`: Perf-Map der übersetzten Funktionen für Profile nach Gastfunktion |
| `fix/dual-screen-frame-wait` | Warten auf das vorherige Bild pro Swapchain statt global |

## Messungen

Hafen, Stock-Takt, Qualcomm-Treiber, Dual Screen, letzte 70 s. Leistung = Akkuleistung des ganzen Geräts.

| Lauf | Ø FPS | p99 | ≤ 16,7 ms | Leistung | CPU max | GPU max |
|---|---|---|---|---|---|---|
| r5 Basis (`dev`) | 55,0 | 23,8 ms | 18,4 % | 9,56 W | 95 °C | 71 °C |
| r5 pipe (Pipelining-Fix) | **59,4** | 23,6 ms | **51,1 %** | lud | 95 °C | 78 °C |
| r5 all (Pipelining + Leerlauf-Schlaf) | **59,7** | **19,5 ms** | 51,0 % | lud | 95 °C | 77 °C |

- Der Pipelining-Fix bringt mit Dual Screen 4,4 FPS.
- Mit Leerlauf-Schlaf taktet der schnellste Kern zwischen 1,84 und 3,19 GHz statt dauerhaft 3,19 GHz. p99 sinkt auf 19,5 ms.
- Während pipe und all hing der Thor am Ladegerät. Der Akkustrom war dann der Ladestrom, die Leistungswerte sind unbrauchbar und gestrichen. Temperaturen sind durch das Laden ebenfalls etwas erhöht. Leistungsmessungen brauchen Akkubetrieb.

## JIT-Profil (r5-all-jit, Kern 1)

- 77 % der Zyklen von Kern 1 liegen im übersetzten Spielcode. Das Profil ist flach: Die 40 heißesten Gastfunktionen machen zusammen nur 19 % aus. Es gibt keine einzelne Schleife, die man gezielt verbessern könnte; der Hebel ist die allgemeine Qualität des erzeugten Codes.
- Verteilung der Samples im JIT-Code:

| Anteil | Ursache |
|---|---|
| **28 %** | **Zykluszähler `remainingCycles`**: pro Block `ldr` → `sub` → `str` über den Speicher, die Schleifenprüfung lädt ihn sofort wieder |
| 13,5 % | Gast-Register aus dem/in den Kontext laden/speichern (Blockgrenzen) |
| 10,4 % | `fmov`/`fcvt` (Float-Umwandlungen, GPR↔FPR-Transfers) |
| 10,0 % | `rev` (Byte-Tausch, Big Endian) |
| 6,7 % | `cmp`/`cset` (ein PPC-`cmpw` wurde zu 3× `cmp` + 3× `cset`) |
| 5,1 % | indirekte Sprünge (`br`) |
| 3,1 % | echte Gast-Speicherzugriffe |

- Umgesetzt auf `perf/aarch64-codegen`:
  1. Kürzere Speicher- und Logikbefehle (Offset 0 ohne `add`, `rev16`, Floats direkt ins FP-Register, Masken als Immediate)
  2. Vergleich + Sprung über die Flags (`cmp` + `b.cond`, der vorhandene x64-Pass funktioniert auch für AArch64)
  3. Ein Befehlsplatz pro Sprung statt zwei (kein `nop` mehr hinter fast jedem Sprung)
  4. **Zykluszähler in `x24`** statt im Speicher, zurückgeschrieben nur vor HLE-Aufrufen und beim Verlassen; gleiche Vergleiche teilen sich ein `cmp`
- Alle vier Stufen laufen fehlerfrei durch den Hafen-Ablauf. Messung gegen dieselbe Referenz (System-Treiber, Akku):

| Lauf | Ø FPS | p99 | Leistung | CPU max | Ø Takt schnellster Kern |
|---|---|---|---|---|---|
| r6-stock (Stufe 1 + 2) | 59,9 | 18,3 ms | 9,71 W | 94,3 °C | 2638 MHz |
| **r7-cg4 (Stufe 1–4)** | 59,8 | 18,8 ms | **9,00 W** | **82,4 °C** | **2297 MHz** |

  Die Last von Kern 1 bleibt bei rund 83 %, weil der Governor den Takt für die geringere Arbeit senkt. Das ist der Effizienzgewinn: 0,7 W weniger und 12 °C kühler.

## Treiber (Hafen, Build cg2, Akku)

| Treiber | Ø FPS | p99 | GPU-Thread-Last | Kern-1-Last | Leistung |
|---|---|---|---|---|---|
| Qualcomm System-Treiber V@0676.53 | 59,9 | 18,3 ms | 73 % | 82 % | 9,71 W |
| Qualcomm 819.2 (tatsächlich der System-Treiber, siehe unten) | 59,5 | 23,3 ms | 72 % | 82 % | 9,43 W |
| **Turnip 27.0.0-R2** | 59,3 | 21,4 ms | **51 %** | 82 % | 9,80 W |
| **Qualcomm 819.2 (mit Lade-Fix)** | 59,9 | 18,1 ms | 65 % | 84 % | **8,88 W** |
| Qualcomm 837.6 (mit Lade-Fix) | 59,9 | 18,1 ms | 67 % | 84 % | 9,11 W |

- Turnip 27.0.0-R2 ist deutlich besser als Turnip T30 (44–49 FPS) und senkt die CPU-Last des GPU-Threads um rund 20 Prozentpunkte. p99 ist etwas schlechter (21,4 gegen 18,3 ms), zwei Bilder über 50 ms.
- **Fehler im Treiber-Laden:** Cemu kopierte nur die Hauptbibliothek eines Treiberpakets in den internen Speicher. Qualcomm-Pakete bringen eigene Versionen ihrer Abhängigkeiten mit (`notgsl.so`, `notadreno_utils.so`, `notllvm-glnext.so`), die dadurch fehlten. Das Log meldete trotzdem „Loaded custom driver“, im Einsatz war aber der System-Treiber V@0676.53 vom Dezember 2023. Fix auf `fix/custom-driver-package`, Nachmessung r7-qcom819/837 läuft.
- Turnip Gen8 V39 ist für A8xx gebaut und wurde nicht getestet. Qualcomm 837.6 verlangt laut Metadaten Android 15 (API 35), läuft auf dem Thor mit Android 13 aber trotzdem.
- ~~Empfehlung: Qualcomm 819.2~~ **zurückgezogen**: 819.2 erzeugt auf dem Thor kaputte (fast weiße) Bilder, siehe „Flackern“. Der Leistungsvorteil kann zum Teil daher kommen, dass weniger gerendert wurde. Alle Leistungswerte aus Läufen mit 819.2 (r7-qcom819, r8-*, r9-*) sind deshalb nur unter Vorbehalt vergleichbar.

## DDR läuft immer mit Höchsttakt

- Der Speichertakt steht während der ganzen Messung auf 4224 MHz, dem Maximum von LPDDR5X. Der Bandbreitenmonitor fordert nur 2657 MHz an.
- Ursache: Der Memlat-Voter der mittleren Kerne (`memlat:ddr:gold`) hat `min_freq = max_freq = 4224000`. Das gilt auch, wenn Cocoon im Vordergrund ist und Cemu nicht läuft, ist also eine Systemeinstellung (vermutlich Leistungsprofil), kein Spiele-Boost.
- Das kostet dauerhaft Leistung, auch im Leerlauf. Ändern kann es nur der Nutzer über das Leistungsprofil (Root nötig für sysfs, wird hier nicht angefasst).

## GamePad-Bildrate

- Test mit fest 10 FPS auf dem zweiten Bildschirm: 9,37 W statt 9,71 W (−0,34 W), CPU im Messfenster 0,2 °C kühler, TV unverändert 59,8 FPS. Gespart werden GPU-Arbeit für Kopie und Present sowie Arbeit im Display-Compositor; der GPU-Thread wird kaum leichter.
- Umgesetzt auf `feature/gamepad-frame-rate`, Einstellung „GamePad-Bildrate“:
  - **Immer voll**: wie bisher
  - **Voll, pausiert bei unverändertem Bild**
  - **Automatisch** (Standard): 30 FPS, pausiert bei unverändertem Bild
  - **Niedrig**: 10 FPS, pausiert bei unverändertem Bild
  - Volle Rate für zwei Sekunden nach jeder Berührung des GamePad-Bildschirms und solange das GamePad auf dem großen Bildschirm liegt.
- Erkennung unveränderter Bilder: Ein kleiner Shader bildet pro Bild einen Hash über jede 8×8-Kachel des GamePad-Bildes (exakte Bitwerte), die CPU vergleicht ihn ein bis zwei Bilder später mit dem vorherigen. Nach 0,5 s ohne Änderung wird nicht mehr präsentiert, bis sich etwas ändert.
- Auf dem Thor geprüft: Am Titelbild präsentiert die GamePad-Ebene laut SurfaceFlinger keine Bilder mehr (letztes Bild 8,6 s älter als das des TV), nach einer Änderung wieder.

## Taktbegrenzung (uclamp)

- Idee: Mit fester Bildzeit wäre der niedrigste Takt, der die 16,7 ms gerade schafft, effizienter als die Spitzen des Governors.
- `sched_setattr` mit `SCHED_FLAG_UTIL_CLAMP_MAX` aus der App: „Operation not permitted“. Ohne Root nicht nutzbar. Offen bleibt die ADPF-Schnittstelle (Performance Hints).
- Streuung zweier gleicher Builds (r8-dev-819 und r8-clamp-819, wirkungslos): 8,82 gegen 9,01 W, also rund 0,2 W.

## Grundlast und Ziel

- Cocoon im Leerlauf, beide Bildschirme an (Helligkeit 184/255): **2,08 W**. Darin steckt auch der dauerhaft auf Höchsttakt stehende DDR-Speicher.
- Ziel unter 7 W heißt: Der Emulator darf rund 4,9 W zusätzlich verbrauchen.

## Rücksprung-Vorhersage (perf/return-prediction)

- Gast-Aufrufe liefen über `br`, Rücksprünge damit über die indirekte Sprungvorhersage. Jetzt `blr` für Aufrufe und `ret` für Rücksprünge, damit greift der Return-Address-Stack der CPU. Die Rückkehradresse aus dem JIT-Code liegt dafür auf dem Stack statt in x30.
- r9-ret (dev + Rücksprung-Vorhersage): **8,23 W** (dev-Läufe 8,82/9,01 W), CPU im Messfenster 67,4 °C statt 71–72 °C, Spiel fehlerfrei. Weniger Fehlvorhersagen heißt weniger spekulativ verworfene Arbeit bei gleichem Takt.

## Code-Layout (perf/code-arena)

- Profil r9-padauto-jit: 20,7 % der JIT-Samples landen auf dem ersten Befehl einer Funktion (meist `sub w24`), 13,8 Prozentpunkte davon am Funktionsanfang selbst. Das sind die Kosten des Sprungs in die Funktion, nicht der Befehl.
- Ursache: Jede der 23.861 übersetzten Funktionen hatte eine eigene Allokation und begann auf einer eigenen 4-KB-Seite (Median-Abstand der Funktionsanfänge genau 4096 Byte, 125 MB Puffer, großteils Füllung). Jeder Aufruf traf eine neue Seite (iTLB), und alle Funktionsanfänge lagen auf demselben Seiten-Offset (dieselben Cache-Sätze).
- Fix: Funktionen werden in 4-MB-Blöcke ausführbaren Speichers kopiert, auf Cache-Zeilen ausgerichtet. Fallback auf getrennte Allokationen, falls kein solcher Speicher gemappt werden kann.

## Flackern mit Dual Screen (fix/backbuffer-quad-scissor)

- Beobachtung des Nutzers: Welt, Schatten und FPS-Anzeige flackern leicht und unregelmäßig.
- Ursache: Das GamePad-Bild wird in den zweiten Bildschirm kopiert, sobald das Spiel es kopiert, also mitten im Bild. Dabei wurde das Scissor-Rechteck auf die Fenstergröße gesetzt und nicht zurückgestellt. Cemu setzt es nur neu, wenn das Spiel seine Scissor-Register ändert. Bis dahin wurden die folgenden Draws abgeschnitten. Meine Kachel-Hash-Prüfung für unveränderte GamePad-Bilder hatte denselben Fehler mit einem viel kleineren Rechteck und verstärkte es stark.
- Fix: Viewport und Scissor werden nach beiden Durchgängen wiederhergestellt, die Deskriptor-Sets beim nächsten Draw neu gebunden.
- Prüfung mit Bildschirmaufnahme einer ruhenden Szene (`flicker_test.sh`, 15 s, weiße Bilder gezählt):

| Build | Treiber | weiße Bilder |
|---|---|---|
| dev (alter Fehler in der Fensterkopie) | Qualcomm 819.2 | 56 % |
| GamePad-Branch mit Scissor-Fix | Qualcomm 819.2 | 14 % |
| GamePad-Branch mit Scissor-Fix | System-Treiber | **0 %** (897 Bilder, mittlere Differenz 0,08) |

- **Hauptursache ist der Quest-Treiber 819.2**, der Scissor-Fehler kam dazu. Fix ist in `dev`.
- Weitere Treiber mit demselben Test (Bilder mit über 90 % weißen Pixeln, 15 s):

| Treiber | weiße Bilder | Helligkeit min/Median/max |
|---|---|---|
| System-Treiber | 0 von 897 | 101,8 / 102,0 / 102,3 |
| Turnip 27.0.0-R2 | 0 von 898 | 101,8 / 102,1 / 102,5 |
| Qualcomm 837.6 | **416 von 897** | 76,6 / 248,9 / 255,0 |

- **Empfehlung:** System-Treiber oder Turnip 27.0.0-R2. Beide Quest-Treiber (819.2, 837.6) sind auf dem Thor unbrauchbar.

## Rücksprung-Vorhersage und Code-Layout mit System-Treiber

Saubere Vergleichsmessung ohne Quest-Treiber (Hafen, Akku):

| Lauf | FPS | p99 | Leistung | Kern 1 | Prime-Takt in der Szene |
|---|---|---|---|---|---|
| r10-dev-stock | 59,7 | 18,8 ms | 8,94 W | 82 % | 2047 MHz |
| r10-next-stock (+ Rücksprung-Vorhersage, Code-Layout) | 59,8 | 18,3 ms | **8,78 W** | 80 % | 1954 MHz |

- Kleiner, aber in allen Größen gleichgerichteter Gewinn (Streuung rund 0,2 W). Beide Branches sind in `dev` gemergt (f7d2918d).
- Die früheren 8,23 W aus r9-ret liefen mit 819.2 und sind nicht vergleichbar.

## LLVM-Stufe (feature/llvm-recompiler)

- LLVM 23.1.4 für Android arm64 gebaut (nur AArch64-Target, 3 GB installiert, statische Bibliotheken).
- Neues Backend übersetzt die IML einer Funktion vor der Registervergabe nach LLVM IR, optimiert mit O2 und übersetzt mit ORC. Gast-Register werden lokale Variablen, nur geänderte werden am Ende zurückgeschrieben. Gast-Speicherzugriffe bleiben `volatile` (andere Kerne und die GPU lesen mit).
- Das AArch64-Backend erzeugt pro Einsprungpunkt einen kleinen Thunk, der die LLVM-Funktion aufruft und über die Sprungtabelle weitermacht. Jump-Table, Invalidierung und Perf-Map bleiben dadurch unverändert. Funktionen mit HLE-Aufrufen oder unbekannten Operationen bleiben beim JIT.
- Übersetzte Funktionen liegen dicht gepackt in eigenem ausführbarem Speicher (eigener ORC-Speichermanager), nicht eine Seite pro Funktion.
- Einschalten über `llvm.enable` im Benutzerordner, Bauen mit `LLVM_DIR=…/lib/cmake/llvm ./gradlew assembleProfile`.
- **Stufenweise Übersetzung:** Der JIT übersetzt jede Funktion sofort wie bisher, LLVM übersetzt sie danach im Hintergrund (niedrige Priorität, kleine Kerne) und übernimmt ihre Einsprungpunkte. Beide Backends haben an Einsprungpunkten denselben Registerzustand und können sich gegenseitig aufrufen.
- **Cache:** Übersetzte Funktionen werden als Objektdateien pro Titel gespeichert (`cache/llvm/<titleid>.bin`, Schlüssel ist ein Hash des Gast-Codes und der Einsprungpunkte). Ab dem zweiten Start nimmt der Recompiler-Thread die LLVM-Fassung direkt.
- Nur Register, die nach einem Einsprungpunkt gelesen werden, werden dort geladen (Liveness über die IML-Segmente). Stack-Zugriffe über r1 sind nicht `volatile`, damit LLVM sie paaren und weiterreichen kann.

Funktionstests (Hafen, System-Treiber, 15 s Aufnahme):

| Build | per LLVM übersetzt | Übersetzungszeit | Bild |
|---|---|---|---|
| llvm1 (LLVM beim ersten Aufruf, Flag-Sprünge noch abgelehnt) | 7.000 | 4 ms | 898 Bilder, 0 fehlerhaft |
| llvm2 (alle Operationen, Stack nicht volatile) | 18.000 | 17 ms | 902 Bilder, 0 fehlerhaft, aber 28 FPS beim Eintreffen, weil neuer Code bis zur Übersetzung interpretiert wird |
| llvm4 (stufenweise, Liveness) | läuft im Hintergrund | 40 ms (kleiner Kern) | 57 FPS sofort im Hafen |

Erste Leistungsmessungen (Hafen, System-Treiber, Akku):

| Lauf | FPS | Leistung | Kern 1 | Prime-Takt | Bemerkung |
|---|---|---|---|---|---|
| r10-next-stock (JIT) | 59,8 | 8,78 W | 80 % | 1954 MHz | Referenz |
| r11-llvm-warm | 38,4 | 9,23 W | 98 % | 3187 MHz | Fehler: jeder Gast-Rücksprung verließ den Recompiler |
| r12-llvm-warm | 59,2 | 11,04 W | 91 % | 3187 MHz | nach der Übersetzung, Gerät 93 °C heiß |
| r13-llvm-perf (mit simpleperf) | 59,3 | 10,20 W | 87 % | 3187 MHz | Cache warm |
| r13-native-perf (mit simpleperf) | 59,8 | 9,26 W | 78 % | 2179 MHz | gleiches APK ohne LLVM |

- Eine unabhängige Prüfung des Backends fand fünf Fehler: Rücksprünge (Code 3) wurden wegen Prüfung nur von Bit 0 als „Recompiler verlassen“ behandelt, `SetEndOfFile` löst unter Android SIGTRAP aus (Absturz beim zweiten Start mit Cache), absolute Adressen der C++-Helfer im Cache (ASLR), unvollständiger Cache-Schlüssel, `xor rX,rX,rX` auf ungeladenem Register. Alle behoben.
- Profil r13 (Kern 1, 20 s): LLVM-Funktionen 33,7 Mrd. Takte gegenüber 36,2 Mrd. für den gesamten JIT-Code nativ, der Code selbst ist also rund 7 % besser. Die Einsprung-Thunks kosten aber weitere 12,1 Mrd. (23,6 %). Übergänge zwischen Funktionen fressen den Gewinn.
- Daraus folgt Stufe 3: Gast-Aufrufe zwischen LLVM-Funktionen werden echte Host-Aufrufe (Cache pro Aufrufstelle, Generationszähler für Invalidierung, Tiefengrenze 48). Funktionen nutzen `preserve_none`, damit Einsprünge keine Register sichern.

## Offen
- Gast-Register an Blockgrenzen (13,5 %), Float-Umwandlungen (10 %).

## Untertakt als neue Referenz (ab 10.11.)

Der Nutzer spielt mit Untertakt über ClusterTune (Mittelkerne 2054 MHz, Prime 2093 MHz). Alle neuen Messungen laufen damit.

| Lauf | FPS | Leistung | Kern 1 | GPU-Thread | Kern 2 | Szene |
|---|---|---|---|---|---|---|
| r21-uc-base (dev) | 59,9 | 7,92 W | 79 % | 81 % | 42 % | 66,8 °C |
| r21-uc-padauto (GamePad automatisch) | 59,8 | 8,14 W | 79 % | 81 % | 42 % | 67,5 °C |
| r21-uc-pools (Command-Pool je Puffer) | 59,9 | 8,14 W | 78 % | 80 % | 42 % | 67,1 °C |
| r20-uc-llvm (LLVM, direkte Aufrufe) | 58,8 | 8,11 W | 89 % | 80 % | 53 % | 70,7 °C |
| r22-uc-gpu550 (dev, GPU max 550 MHz) | 58,9 | 7,80 W | 79 % | 82 % | 42 % | 67,0 °C |

- Streuung zweier gleicher Läufe rund ±0,15 W.
- DDR steht immer auf 4224 MHz: Ein Speicher-Voter (`memlat:ddr:gold`) wird alle 10 s vom Qualcomm-Perf-HAL neu auf 4,2 GHz gesetzt, in jedem AYN-Leistungs- und Lüftermodus, auch ohne ClusterTune. Firmware-Verhalten, ohne root nicht änderbar.
- Command-Pools: `vkResetCommandPool` kostet im Adreno-Treiber genauso viel wie `vkResetCommandBuffer` (46 % des Submit-Pfads), kein Gewinn. GamePad automatisch: In dieser Szene ändert sich die Karte ständig, die Bildänderungs-Erkennung kostet so viel wie sie spart.
- GPU-Thread: 38 % der Emulator-CPU-Zeit. Submit-Pfad 19 % des Threads, davon die Hälfte Command-Buffer-Reset im Treiber.

## Render-Passes (perf/renderpass-reuse)

- Die GPU ist in der Hafenszene zu 73 % ausgelastet, meist auf 680 MHz, obwohl LCU nativ in 720p rendert. Ein GPU-Limit von 550 MHz kostet 1 FPS. Ursache: zu viele Render-Passes. Adreno lädt bei jedem Pass-Beginn Farbe und Tiefe aus dem Speicher und schreibt sie am Ende zurück.
- Zählung pro Bild: ~960 Draw-Calls, ~120 Render-Passes. 86 Wechsel des Render-Targets, davon ~39 Teilmenge und ~33 Obermenge des laufenden Passes: Das Spiel wechselt zwischen Draw-Calls mit einem und zwei Farbausgängen, und Cemu baut das Framebuffer nur aus den Ausgängen, die der Pixel-Shader schreibt.
- Fix: Der laufende Pass bleibt, wenn er alle Attachments des neuen Framebuffers enthält und der Draw-Call keines der übrigen als Textur liest. Farb-Attachments ohne Shader-Ausgang bekommen Schreibmaske 0. Ergebnis: **~60 statt ~120 Render-Passes pro Bild**, Flackertest sauber (895 Bilder, 0 fehlerhaft).

## GPU-Zeit pro Render-Pass (perf/gpu-timing)

- Die GPU ist in der Hafenszene fast voll ausgelastet: 89 % bei 670 MHz (Stock) bzw. 93 % bei 550 MHz (GPU-Limit). Frühere Werte von 73–77 % waren Mittel über den ganzen Lauf inklusive Laden.
- Messwerkzeug: Zeitstempel (`vkCmdWriteTimestamp`) vor jedem Render-Pass, nach jedem Pass-Ende und um Kopien und Clears herum. Eingeschaltet über die Datei `vk.gputime`, Ausgabe alle 600 Bilder im Log, gruppiert nach Framebuffer (Größe, Formate, Adressen).
- Hafenszene, System-Treiber, GPU-Limit 550 MHz: **15,1 ms GPU-Zeit pro Bild**:

| ms/Bild | pro Bild | Was |
|---|---|---|
| 5,0 | 4,9× | Haupt-Pass 1280×720, drei Farbziele + Tiefe, 535 Draws |
| **2,6** | **23,6×** | **Farb-Clears (`vkCmdClearColorImage`)** |
| 1,9 | 12,3× | Pass Farbe 0 + Tiefe, 103 Draws |
| **0,85** | **12,7×** | **Tiefen-Clears** |
| 1,0 | 3× | Schattenkarten 960×960 |
| 0,37 / 0,17 | 1× | Hochskalierung TV (bikubisch, 1920×1080) / GamePad |
| 0,27 | 2× | Tiefen-Kopie per Draw |

- **Clears kosten 23 % der GPU-Zeit.** Sie laufen außerhalb von Render-Passes, schreiben das ganze Bild in den Speicher, und der nächste Pass lädt es wieder. Auf einer Kachel-GPU kostet ein Clear im Pass fast nichts.

## Verzögerte Clears (perf/deferred-clears)

- Ein Clear wird an der Textur vermerkt statt sofort ausgeführt. Beginnt der nächste Render-Pass, der in die Textur rendert, wird er als `LOAD_OP_CLEAR` dieses Passes ausgeführt (kompatible Render-Pass-Variante, die alten Inhalte werden nicht geladen). Ist die Textur Teil des laufenden Passes, gibt es `vkCmdClearAttachments` im Pass. Jede andere Nutzung führt den Clear vorher aus: Sampeln, Kopien, Uploads, Readback, Präsentieren. Ein zweiter Clear derselben Ebene ersetzt den ersten.
- Hafen: Pro Bild laufen 18,5 Clears im Render-Pass, 1 separat, und **23 Clears fallen ganz weg**, weil das Spiel dasselbe Ziel vor jeder Nutzung erneut cleart.
- Flackertest (System-Treiber) beide Stufen sauber: 892 bzw. 891 Bilder, 0 fehlerhaft.

| Lauf | GPU-Zeit/Bild | GPU-Takt (Szene) | GPU-Last | GPU-Arbeit | FPS | p99 |
|---|---|---|---|---|---|---|
| r26 Render-Pass-Fix | 15,1 ms | 550 MHz | 92,6 % | 509 Mcyc/s | 59,5 | 20,9 ms |
| r29 + verzögerte Clears | 13,2 ms | 513 MHz | 78,6 % | 405 Mcyc/s | 59,9 | 18,2 ms |
| r31 + LOAD_OP_CLEAR | 12,6 ms | 456 MHz | 79,5 % | 363 Mcyc/s | 59,9 | 18,2 ms |

- **GPU-Arbeit −29 %.** Die GPU läuft jetzt meist mit 475 oder 401 MHz statt durchgehend mit 550 MHz. Render-Passes pro Bild: 51 (ursprünglich 120).
- Leistung am Ladegerät nur als Tendenz: 8,55 → 7,57 W. Saubere Messung bei vollem Akku folgt.

## Genaue Shader-Multiplikation

- LCU nutzt die Voreinstellung „genaue Multiplikation“: `0 × alles = 0` wird bei jeder Multiplikation von Registern geprüft.
- Komplett ausgeschaltet (Spielprofil): Haupt-Pass 5,1 → 3,9 ms. Achtung: Cemus SPIR-V-Cache berücksichtigt diese Option nicht. Nur Shader, die neu übersetzt werden, ändern sich.
- Günstigere Prüfung nur des Ergebnisses (`isnan(a*b) ? 0 : a*b`, frischer SPIR-V-Cache): nur −1 %. Der Preis liegt also nicht in den Vergleichen, sondern vermutlich darin, dass Multiplikation und Addition nicht mehr zu FMA verschmolzen werden.
- Bildvergleich im Stand (Flackeraufnahme, frischer Cache): Unterschiede nur auf animierten Elementen (drehende Steine, Figur, HUD, Verkehr), statische Geometrie identisch. Andere Level ungeprüft.
- Gegenprobe mit frischem SPIR-V-Cache: Ohne genaue Multiplikation sinkt die GPU-Arbeit um 8 % (333 statt 363 Mcyc/s), Haupt-Pass 5,05 → 4,6 ms. Fusioniertes Multiply-Add mit Prüfung (`muladd_nonIEEE`) bringt nichts. Die Prüfung selbst kostet also wenig, ihr Preis liegt in Optimierungen, die der Shader-Compiler nur ohne sie machen kann. **Option für den Nutzer:** Spielprofil `accurateShaderMul = false`, mögliche Bildfehler in einzelnen Szenen, nicht standardmäßig aktiviert. Branch `perf/strict-mul` nicht gemergt.

## Hochskalierung durch den Display-Controller (perf/compositor-scaling)

- Android skaliert eine kleinere Swapchain auf die Fenstergröße. Die Swapchain ist jetzt nur so groß, dass das Spielbild 1:1 hineinpasst: TV 1280×720 statt 1920×1080, GamePad 856×746 statt 1240×1080. Der Compositor skaliert in der Display-Hardware (SurfaceFlinger: „Device“-Komposition für beide Ebenen).
- Touch-Koordinaten bleiben in Fensterkoordinaten, nur das Zeichnen des Bildes rechnet in Swapchain-Pixeln.
- Hafen: GPU-Arbeit −5 % (346 statt 363 Mcyc/s), Szene 1,8 °C kühler. Flackertest sauber, Bild im Ausschnittvergleich mit bikubischer GPU-Skalierung praktisch gleich.

## Leistung am Ladegerät bei vollem Akku (10.11., 03:07–03:30)

Hafen, Untertakt (Prime 2093, Mitte 2054 MHz), GPU-Limit 550 MHz, System-Treiber, abwechselnd gemessen. Leistung = USB-Eingang minus Akku-Ladeleistung, Ladeverluste sind enthalten.

| Lauf | Stand | Akku | Leistung | FPS | p99 | CPU max | Szene | GPU-Arbeit |
|---|---|---|---|---|---|---|---|---|
| p1-1 | alter dev | 99 % | 8,29 W | 59,2 | 22,9 ms | 74,5 °C | 68,8 °C | 512 Mcyc/s |
| p1-2 | neuer dev | 99 % | 7,33 W | 59,7 | 19,9 ms | 67,7 °C | 64,1 °C | 379 Mcyc/s |
| p1-3 | alter dev | 100 % | 8,12 W | 59,1 | 22,7 ms | 76,1 °C | 68,8 °C | |
| p1-4 | neuer dev | 100 % | **6,69 W** | 59,8 | 18,3 ms | 68,9 °C | 62,8 °C | |

- **−1,2 bis −1,4 W (−15 bis −18 %)**, 4–6 °C kühler, FPS und p99 besser. Neuer dev = Render-Pass-Wiederverwendung + verzögerte Clears mit LOAD_OP_CLEAR + Hochskalierung durch den Display-Controller.
- Am Ladegerät liegt der Wert rund 0,3 W über der Akkumessung (alter dev: 7,80 W auf Akku, 8,12 W hier). Der neue Stand entspricht damit etwa **6,4 W auf Akku**.

## Turnip V39 (Gen8 V39, Mesa 27.0.0-devel)

- Läuft auf dem Adreno 740, beide Varianten (normal, patched) im Flackertest sauber (898/897 Bilder, 0 fehlerhaft).
- Mit dem neuen dev: GPU-Thread 57 % statt 73–75 %, GPU-Arbeit 332 statt 357–379 Mcyc/s, GPU meist bei 410 MHz. **Aber nur 57,4 FPS**, nur 35 % der Bilder ≤ 16,7 ms. Leistung 6,81 W gegen 6,69–6,91 W mit System-Treiber, also kein Gewinn.
- Off-CPU-Profil: Der GPU-Thread ist unter V39 41 % der Zeit nicht auf der CPU (System-Treiber 31 %), mit vielen Pausen von 2–10 ms mitten in Arbeit. Zusätzlich hält die GPU-Taktsteuerung (DCVS) die GPU unter Turnip auf niedrigerem Takt, Bilder mit mehr GPU-Last werden dann zu spät fertig. Der Qualcomm-Treiber gibt der Taktsteuerung offenbar Hinweise, die Turnip fehlen. Ohne Root (Mindesttakt) nicht lösbar. **Empfehlung: System-Treiber.**

## Messung am Ladegerät bei vollem Akku: Vorsicht

- Bei 100 % meldet der Akku „Full“ und `current_now` exakt 0, auch wenn der Lader nachlädt. Derselbe Build schwankte so zwischen 6,46 und 7,20 W, und die Werte driften mit der Zeit nach unten. Belastbar sind nur abwechselnd gemessene Vergleiche (A-B-A-B) und Stellvertreter-Größen: GPU-Arbeit, Thread-Last, Takte, Temperaturen.

## GPU-Thread entlasten (perf/index-cache, perf/async-cmd-reset)

- **Index-Cache:** Hatte 8 Einträge und wurde bei jedem fertigen Command-Buffer geleert, fast jeder Draw wandelte seine Indizes neu um (8 % des GPU-Threads). Jetzt bleiben umgewandelte Indizes über Bilder erhalten. Nach einem GPU-Warten wird ein Eintrag nur wiederverwendet, wenn eine Prüfsumme der Gastdaten gleich ist. Flackertest sauber. A-B-A-B bei vollem Akku: dev2 7,84/7,28 W, Index-Cache 7,20/6,83 W (Drift berücksichtigt etwa −0,1 bis −0,3 W).
- **Command-Buffer-Reset auf eigenem Thread:** Ein Pool pro Command-Buffer, fertige Buffer setzt ein Thread auf den kleinen Kernen zurück (vorher 6 % des GPU-Threads).
- **Weniger Submits:** Für jedes geholte Swapchain-Bild (TV, GamePad) gab es einen eigenen Submit, nur um auf das Bild zu warten. Das Warten hängt jetzt am nächsten regulären Submit (Stufe Color-Attachment-Output).

## Gesamtergebnis der Nacht (schneller Leistungslogger, 10.11. 05:00–05:20)

Hafen, Untertakt, GPU-Limit 550 MHz, System-Treiber, am Ladegerät bei vollem Akku, abwechselnd gemessen. Leistung = USB-Eingang, gemittelt über ca. 300 Werte pro Szene (`power_log.py`), zwei gleiche Läufe liegen damit nur noch ±0,05 W auseinander.

| Lauf | Stand | Leistung | FPS | p99 | ≤16,7 ms | GPU-Arbeit | GPU-Thread | Szene |
|---|---|---|---|---|---|---|---|---|
| p6-1 | alter dev | 7,58 W | 59,3 | 21,7 ms | 48 % | 516 Mcyc/s | 81 % | 67,7 °C |
| p6-2 | neuer dev | 6,61 W | 59,9 | 18,1 ms | 51 % | 357 Mcyc/s | 69 % | 62,9 °C |
| p6-3 | alter dev | 7,72 W | 58,9 | 23,1 ms | 46 % | 510 Mcyc/s | 82 % | 68,2 °C |
| p6-4 | neuer dev | 6,65 W | 59,9 | 18,1 ms | 51 % | 383 Mcyc/s | 70 % | 64,2 °C |

- **Mittel: 7,65 W → 6,63 W, −1,02 W (−13 %)**, 4,4 °C kühler, FPS gepinnt bei 59,9, p99 von 22,4 auf 18,1 ms.
- Neuer dev = Render-Pass-Wiederverwendung, verzögerte Clears mit LOAD_OP_CLEAR, Hochskalierung durch den Display-Controller, Index-Cache, Command-Buffer-Reset auf eigenem Thread, weniger Submits.
- Einzelbeitrag der drei GPU-Thread-Änderungen (p5): 6,93 → 6,76 W.
- Das Ziel unter 6 W ist damit noch nicht erreicht, es fehlen rund 0,6 W.
- Zweite unabhängige Serie mit dem finalen dev inkl. GamePad „Automatisch“ (p10): alter dev 7,78 / 7,52 W, neuer dev 6,69 / 6,60 W, also wieder **7,65 → 6,65 W**. FPS 59,9 / 59,8, p99 18,4 / 18,2 ms, Szene 62,6 / 60,5 °C.
- Kürzeres Spinnen vor dem Schlafen (20 statt 50 µs, p9): 6,62 gegen 6,70 W, innerhalb der Streuung. Bleibt bei 50 µs.
- Format-Prüfung für Clears im Render-Pass (View-Format = Bildformat): weiterhin 18,6 Clears pro Bild im Pass, 1 separat. In dev.

## GamePad-Bildrate, jetzt in dev (feature/gamepad-frame-rate)

Mit dem schnellen Logger messbar (neuer dev, Modus per Mess-Override):

| Modus | Leistung | FPS | GPU-Arbeit |
|---|---|---|---|
| Voll | 6,75 W | 59,9 | 381 Mcyc/s |
| Automatisch (30 FPS, Pause bei unverändertem Bild) | 6,57 W | 59,9 | 367 Mcyc/s |
| Niedrig (10 FPS) | 6,54 W | 59,9 | 349 Mcyc/s |

- „Automatisch“ spart 0,18 W bei flüssiger Karte. Die frühere Messung (r21) sah keinen Gewinn, weil 8 Messwerte pro Szene zu wenig waren. Branch in dev gemergt, Standard ist „Automatisch“.

## Was die CPU-Takte bestimmt

- In allen Läufen stehen Prime- und Mittelkerne in der Szene fast immer auf ihrer ClusterTune-Obergrenze (2092 bzw. 2054 MHz), auch mit Turnip, wo der GPU-Thread nur 57 % Last hat.
- WALT auf dem Thor: Colocation (zusammengehörige Threads der Vordergrund-App werden gemeinsam bewertet), Busy-Hysterese 8,5 ms auf allen Kernen, Input-Boost 1,23 GHz auf den kleinen Kernen. Systemeinstellungen, ohne Root nicht änderbar.
- Folge: Weniger CPU-Arbeit spart nur linear Energie, der Takt bleibt oben. **Die ClusterTune-Obergrenzen sind jetzt der wirksamste CPU-Hebel.** Last bei 2092/2054 MHz: Hauptkern 80 %, GPU-Thread 69 %, Kern 2 44 %. Es gibt also Luft für die nächstniedrigeren Stufen (Prime 1977 oder 1843 MHz, Mitte 1920 oder 1785 MHz). Das ist eine Einstellung des Nutzers.
- Transparent Huge Pages sind im Kernel aus („never“), große Seiten für den Gastspeicher gehen ohne Root nicht.

## Genaue Shader-Multiplikation: Leistung (p8)

Neuer dev, beide Varianten mit frisch übersetztem SPIR-V-Cache:

| Lauf | Variante | Leistung | FPS | p99 | GPU-Arbeit | GPU-Takt |
|---|---|---|---|---|---|---|
| p8-1 | genau | 6,63 W | 59,5 | 21,4 ms | 369 Mcyc/s | 484 MHz |
| p8-2 | schnell | 6,85 W | 59,2 | 25,0 ms | 308 Mcyc/s | 401 MHz |
| p8-3 | genau | 6,75 W | 59,9 | 18,1 ms | 365 Mcyc/s | 475 MHz |
| p8-4 | schnell | **6,30 W** | 59,9 | 18,2 ms | 298 Mcyc/s | 420 MHz |

- p8-2 ist verfälscht: Die neuen SPIR-V-Shader zwingen den Treiber, alle Pipelines während des Spielens neu zu übersetzen (zwei Ruckler über 50 ms, CPU-Last). Ab dem zweiten Start (p8-4) ist der Treiber-Cache warm.
- **Ohne genaue Multiplikation: −0,39 W, GPU-Arbeit −19 %, GPU meist bei 401–420 MHz.** Im Hafen keine sichtbaren Unterschiede. Andere Level sind ungeprüft.

## Weg unter 6 W

Stand dev (`7c11af89`, inkl. GamePad „Automatisch“): rund 6,5 W am Ladegerät. Was noch fehlt, ist eine Entscheidung des Nutzers, kein offener Code:

1. **Spielprofil „Shader multiplication accuracy“ aus** (LCU, Spielprofil in der App): −0,39 W, GPU −19 %. Im Hafen kein sichtbarer Unterschied, andere Level ungeprüft. Der erste Start danach übersetzt alle Shader und Pipelines neu (Ruckler), danach ist es flüssig.
2. **ClusterTune eine Stufe tiefer:** Prime 1977 statt 2093 MHz, Mitte 1920 statt 2054 MHz. Beide Cluster stehen in der Szene fast immer auf der Obergrenze, die Last liegt bei 80 % (Hauptkern) und 69 % (GPU-Thread), es ist also Luft da. Geschätzt −0,1 bis −0,2 W, nicht gemessen (Systemeinstellung).
3. **GPU-Limit 475 statt 550 MHz:** Die GPU braucht nach den Änderungen meist 401–475 MHz, 550 MHz kommen nur noch in Spitzen vor.

Größere Software-Hebel für später:

- JIT: tote Speicherungen von CR0 und flüchtigen Registern an `blr`/`bl` weglassen (CR0-Bytes sind 16 % aller Kontextzugriffe), feste Host-Register für r1/r2/r13. Geschätzt 3–8 % des Hauptkerns.
- Turnip: verdrängter GPU-Thread und Taktsteuerung (siehe oben), ohne Root kaum lösbar.
- DDR-Takt und Transparent Huge Pages: Firmware/Kernel, nur mit Root.

## Finaler Stand mit und ohne genaue Multiplikation (p11, p12)

dev mit GamePad „Automatisch“, frischer SPIR-V-Cache für alle Läufe:

| Variante | Läufe | Mittel | GPU-Arbeit | GPU-Takt | FPS |
|---|---|---|---|---|---|
| genau (Standard) | 6,57 / 6,53 W | 6,55 W | 381 / 358 Mcyc/s | 494 / 466 MHz | 59,9 / 59,8 |
| schnell (Spielprofil) | 6,39 / 6,17 W | **6,28 W** | 308 / 299 Mcyc/s | 420 / 401 MHz | 59,8 / 59,9 |
| Clamp (exp/clamp-mul) | 6,55* / 6,39 W | 6,39 W | 367* / 329 Mcyc/s | 456* / 429 MHz | 59,8 / 59,9 |

\\* erster Lauf mit neuen Pipelines, verfälscht.

- **Mit der Spielprofil-Option: 7,65 → 6,28 W (−1,37 W, −18 %).**
- Clamp-Variante: Beide Faktoren werden auf ±FLT_MAX begrenzt, dann ist 0 × ∞ = 0 ohne Auswahl, und der Compiler sieht eine normale Multiplikation. Etwa die Hälfte der Ersparnis bei fast gleicher Genauigkeit. Abweichungen: NaN × x ≠ 0 ergibt ±∞ statt NaN, ∞ × |x| < 1 ergibt einen großen endlichen Wert statt ∞. Nur ein sauberer Lauf, nicht in dev.

## Release-Vorbereitung Cidemu 0.2 (2026-11-10)

Branch `build/cidemu-release` (auf dev), App-ID `com.cidix.cidemu`, Release-Build läuft die Emulation in `:EmulationProcess`.

- Release gegen Profil-Build (rs1, gleicher nativer Code): 59,9 FPS und p99 18,2 ms bei beiden, Leistung 6,42 / 6,41 W gegen 6,35 / 6,34 W. Der Release-Build liegt in beiden Paaren um 0,07 W höher, Ursache offen (Kotlin minifiziert, zweiter Prozess).
- Review der eigenen Renderer- und JIT-Commits, behobene Funde:
  - erste Löschung einer neuen Textur nicht mehr aufschieben (Layout UNDEFINED)
  - GamePad-Swapchain auch im GPU-Leerlauf abbauen (sonst Hänger des UI-Threads bei pausiertem Spiel)
  - Render-Pass nur behalten, wenn der Draw die zusätzlichen Attachments nicht berührt
  - GamePad-Bild vor der Änderungsprüfung aktualisieren
  - Index-Cache höchstens einmal pro Bild durchsuchen
  - Xbyak-Ausnahmen beim AArch64-Codegen abfangen
- Ein Epoch-Bump des Index-Caches bei jeder TC-Invalidierung kostete im Hafen GPU-Thread-Zeit (LCU invalidiert Uniform-Blöcke ständig) und wurde verworfen. Upstream prüft innerhalb einer Epoche genauso wenig.
- Die strengere Render-Pass-Prüfung lehnte im Hafen rund 21 Wiederverwendungen pro Bild ab (GPU-Arbeit +13 %). Alle waren Draws mit `CB_COLOR_CONTROL.SPECIAL_OP = DISABLE`, also reine Tiefen-Draws. Vorher haben diese im behaltenen Pass tatsächlich Farbe geschrieben. Jetzt maskiert die Pipeline Farbe bei deaktivierter Farbausgabe, und der Pass darf bleiben.

### Cocoon Shell hinter dem GamePad-Bild

- Cocoon Shell (`rip.moth.cocoonshell`, Launcher auf dem unteren Bildschirm) läuft hinter der GamePad-Presentation weiter: rund 21 Bilder/s, 26–37 % eines CPU-Kerns, dazu SurfaceFlinger-Arbeit.
- Eine Presentation ist nur ein Fenster über der Activity des Displays. Mit einer eigenen Activity auf dem Display (`EmulationDisplayActivity`, Branch `perf/pad-activity`) stoppt Android Cocoon: 0 Bilder in 10 s.
- Eine nicht fokussierbare Activity führt zu ANR: Der Start macht das Display zum fokussierten, ohne fokussierbares Fenster hält Android die Gamepad-Eingaben zurück. Die Activity ist deshalb fokussierbar und reicht Tasten und Sticks an die Emulations-Activity weiter.

### Controller-Abfrage im Prozess der Spieleliste

- Ursache für die +0,07 W des Release-Builds gegenüber dem Profil-Build: `InputManager` startet seinen Abfrage-Thread (`Input_update`, 1 ms Takt) in jedem Prozess. Im Release-Build läuft das Spiel in `:EmulationProcess`, der Prozess der Spieleliste fragte trotzdem weiter ab, rund 2 % eines Kerns und 1000 Weckvorgänge pro Sekunde.
- Fix (`ab9b0a64`): Die Abfrage pausiert, solange im Prozess keine Activity gestartet ist. In Builds mit einem Prozess läuft sie während des Spiels weiter, weil die Emulations-Activity gestartet ist.
- Offen: Im Emulationsprozess kostet derselbe Thread ähnlich viel. Android-Controller liefern ihre Werte per Ereignis, ein längeres Intervall oder ereignisgesteuerte Updates wären ein eigenes Experiment.

### Messreihe ab1: alter Stand, Fixes, Display-Activity (Release-Builds, Hafen, je zwei Läufe im Wechsel)

| Stand | Leistung | GPU-Arbeit | FPS | p99 |
|---|---|---|---|---|
| alter Stand (`cidemu-release-test`) | 6,83 / 6,93 W (6,88) | 198 / 182 | 59,9 | 18,1–18,2 ms |
| mit Review-Fixes | 7,05 / 7,01 W (7,03) | 188 / 187 | 59,9 | 18,2–18,4 ms |
| mit Fixes und Display-Activity | 6,75 / 6,76 W (6,76) | 180 / 188 | 59,7–59,9 | 18,3–19,2 ms |

- Display-Activity: −0,27 W gegenüber den Fixes allein, Cocoon zeichnet 0 statt rund 21 Bilder/s.
- Die Fixes liegen bei gleicher GPU-Arbeit und gleichen Thread-Lasten 0,15 W über dem alten Stand. Keinem Thread zuzuordnen, vermutlich Rauschen der versteckten Akku-Nachladung (Akku „voll“, 0 mA).
- Absolutwerte liegen an diesem Nachmittag rund 0,3 W über denen vom Vormittag (gleiches APK). Nur Vergleiche innerhalb einer Reihe zählen.

### Messreihe fin1: finaler Stand gegen alten Stand

| Stand | Leistung | GPU-Arbeit | FPS | p99 |
|---|---|---|---|---|
| final (Fixes, Display-Activity, Controller-Abfrage) | 6,70 / 6,85 W (6,78) | 199 / 170 | 59,9 | 18,1 ms |
| alter Stand | 6,82 / 6,85 W (6,84) | 180 / 183 | 59,9 | 18,0–18,1 ms |

- −0,06 W, im Rauschen. Zusammen mit ab1 (−0,12 W) liegt der finale Stand rund 0,1 W unter dem alten, bei gleichen Bildzeiten. Die −0,27 W aus ab1 gegen den Stand nur mit Fixes waren zu optimistisch, weil jene Läufe hoch lagen.
- Sicher gemessen ist der Effekt auf Cocoon: 0 statt rund 21 Bilder/s und 26–37 % eines Kerns weniger. Ein Leistungsunterschied dieser Größe ist bei ±0,2 W Rauschen durch das Nachladen nur mit mehr Läufen oder im Akkubetrieb sauber zu belegen.

### Schnelle Shader-Multiplikation im Intro

- Unübersprungenes Intro (Ladebildschirm mit drehendem Abzeichen, Zwischensequenzen mit Radio-Untertiteln, Schiff, Chase, Platz mit Menge) mit genauer und mit schneller Multiplikation aufgenommen, die schnelle nach einem Aufwärmlauf für den Shader-Cache.
- Kein sichtbarer Unterschied. Die Aufnahmen sind um etwa 10 s versetzt, weil die schnelle Variante schneller lädt. Das Abzeichen zeigt in beiden Varianten bronzene Vorder- und silberne Rückseite.
- Folge: Das mitgelieferte Spielprofil von LCU (EU `0005000010101b00`, USA `0005000010101a00`) setzt `accurateShaderMul = false`, wie Cemu es für Super Mario 3D World, Captain Toad und Xenoblade X tut. Gemessen waren das −0,27 bis −0,39 W im Hafen.

### Gerätetests des finalen Release-Builds (Thor, `com.cidix.cidemu`)

- Erststart ohne Daten, Spielordner über den System-Dialog freigegeben, LCU erkannt und gestartet.
- Flackertest: 0 Flacker-Frames (vor und nach den Review-Fixes).
- Hintergrund und Rückkehr: kein ANR, Spiel läuft weiter, Cocoon zeichnet im Hintergrund wieder und hält beim Zurückkehren an.
- Spiel beenden über das Seitenmenü: Emulationsprozess endet, Spieleliste oben, Cocoon unten zurück, kein Absturz.
- About: Credits, Quellcode-Link und Lizenzdialog der nativen Bibliotheken.
- Leere Spieleliste ohne Spielpfad: Hinweis mit Knopf zu den Einstellungen.
- Controller-Abfrage: Prozess der Spieleliste 0 Ticks während des Spiels, Emulationsprozess 3 Ticks in 10 s.
- Command-Pools ohne `TRANSIENT_BIT` (Branch `exp/pool-not-transient`, lokal): Reset-Thread 234 statt 220 Ticks in 20 s im Hafen, also kein Gewinn. Verworfen. Der Thread braucht rund 11 % eines kleinen Kerns.

### Akkulauf Cidemu 0.2 gegen den Ausgangspunkt (bat1)

Thor vom Strom getrennt, ClusterTune-Limits aktiv (Prime 2093, Mitte 2054, GPU 550 MHz), Cidemu 0.2 mit LCU-Profil (schnelle Multiplikation), nach einem Aufwärmlauf für den Shader-Cache. Ausgangspunkt ist `r5-base-power` (Standardtakt, ebenfalls Akkuleistung).

| | Ausgangspunkt, Standardtakt | Cidemu 0.2, Limits |
|---|---|---|
| FPS | 55,0 | 59,9 |
| ≤ 16,7 ms | 18,4 % | 52,5 % |
| p99 | 23,8 ms | 18,1 ms |
| Akkuleistung | 9,56 W | **6,07 W (−37 %)** |
| CPU max / Szene | 95,1 / 86,1 °C | 70,1 / 61,9 °C |
| GPU max | 71,1 °C | 57,3 °C |
| GPU-Takt / Last | 477 MHz | 401 MHz / 40 % |

- Ein Lauf. Im Akkubetrieb fällt das Rauschen durch das versteckte Nachladen weg, die Werte sind direkt mit dem Ausgangspunkt vergleichbar.
- Ziel „unter 6 W“: 6,07 W, also knapp darüber.

## Frontends (Cocoon, ES-DE)

- Frontends starten die Emulations-Activity direkt mit der Spieldatei als Content-URI. Für Cidemu: `dev.cidix.cidemu/info.cemu.cemu.emulation.EmulationActivity`, voller Klassenname, weil die Activity im Paket `info.cemu.cemu` liegt. Auf dem Thor per `am start` getestet, ohne Action (wie Cocoon) und mit VIEW (wie ES-DE): LCU bootet.
- Cocoon 3.11: Player nur aus dem Katalog (KansoMirai/CocoonFE, `android/*.yaml`), eigene Player in der App noch nicht (IDs `custom.` sind reserviert). Import von Daijishō-Plattformdateien geht aber: `dist/frontends/NintendoWiiU-Cidemu.json` (ab 0.3 als Release-Asset). Auf dem Thor importiert, Cidemu ist Standard-Player der Wii U, CEMU bleibt wählbar.
- Katalog-PR: KansoMirai/CocoonFE#620.
- ES-DE: `custom_systems/es_find_rules.xml` mit Cidemu vor Cemu in der CEMU-Regel, dokumentiert im README, mangels ES-DE auf dem Thor nicht getestet.
- App-ID ab 0.3 `dev.cidix.cidemu` (Domain cidix.dev).
