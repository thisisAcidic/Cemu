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
