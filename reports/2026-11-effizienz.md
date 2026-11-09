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
- **Empfehlung: Qualcomm 819.2** (Treiberstand August 2025). 0,8 W weniger als der System-Treiber, bestes p99.

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
- Fix: Viewport und Scissor werden nach beiden Durchgängen wiederhergestellt, die Deskriptor-Sets beim nächsten Draw neu gebunden. Prüfung mit Bildschirmaufnahme einer ruhenden Szene (Werkzeug `flicker_test.sh`).

## Offen
- Gast-Register an Blockgrenzen (13,5 %), Float-Umwandlungen (10 %).
