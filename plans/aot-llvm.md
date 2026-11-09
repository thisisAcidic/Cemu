# Vorab-Übersetzung mit LLVM (AOT)

## Ziel
Spielcode einmal vor dem Spielen mit einem starken Optimierer übersetzen, das Ergebnis pro Spiel speichern und danach nur noch den vorübersetzten Code ausführen. Erwartung: 1,5- bis 2-mal schnellerer Spielcode auf Kern 1 und 2, deutlich niedrigerer Takt des schnellsten Kerns, keine Übersetzungs-Ruckler mehr. „Um ein Vielfaches“ ist nicht realistisch: Speicherlatenz des Spiels, Byte-Tausch und die Float-Semantik des Espresso bleiben.

## Warum nicht einfach den heutigen JIT vorab laufen lassen
Übersetzen kostet im Spiel heute nichts: Jede Funktion wird einmal in Bruchteilen einer Millisekunde übersetzt und bleibt im RAM, der Recompiler-Thread steht in allen Hafen-Messungen bei 0 %. Mit gleicher Codequalität brächte Vorab-Übersetzen nur das Wegfallen kurzer Hänger beim ersten Erreichen neuen Codes. Gewinn bringt nur bessere Codequalität, und dafür braucht es einen Optimierer, der für einen JIT zu langsam wäre.

## Architektur
1. **Frontend wiederverwenden:** Cemus PPC→IML-Übersetzung ist erprobt. Neues Backend „IML → LLVM IR“ statt eines eigenen PPC-Frontends (IML: rund 50 Operationen, 30 Befehlstypen; das AArch64-Backend hat 1.800 Zeilen).
2. **Gast-Register als lokale Variablen:** Pro genutztem Gast-Register ein `alloca`, Laden aus dem Kontext beim Eintritt, Zurückschreiben vor Aufrufen in C++ und beim Verlassen. LLVM (mem2reg/SROA) hält sie in Host-Registern.
3. **Aufrufe als echte Aufrufe:** Heute springt ein Gast-Aufruf zum Ziel und das Ziel über die Sprungtabelle zurück. Jede Aufrufgrenze zwingt alle Register durch den Speicher (rund 15 % Kontext-Verkehr plus Aufruf-Overhead im Profil). AOT: `bl` wird `call`, `blr` mit erwarteter Rücksprungadresse wird `ret`, sonst Ausstieg über den bisherigen Weg.
   - Mit dem bekannten Aufrufgraphen kann LLVM kleine Funktionen inlinen und über Funktionsgrenzen optimieren.
   - Thread-Wechsel funktionieren weiter: Jeder PPC-Thread läuft auf einer eigenen Fiber mit eigenem Stack, ein Wechsel mitten in verschachtelten Aufrufen ist möglich.
4. **Funktionen finden:** Rekursiver Abstieg ab Einsprungpunkten, Exporten und Relokationen in Datenbereichen (Funktionszeiger, vtables). Was nicht gefunden wird, übernimmt zur Laufzeit der heutige JIT.
5. **Mischbetrieb mit dem JIT:** Ruft AOT-Code eine nicht vorübersetzte Funktion auf, läuft diese im JIT, bis sie zu einer besonderen Rücksprungadresse zurückkehrt, deren Tabelleneintrag zurück in den AOT-Code führt.
6. **Zykluszähler:** Prüfung an Schleifenrücksprüngen und Aufrufen wie heute, Zähler in einem Register.
7. **Cache:** Ergebnis pro Titel-ID, Version und Cidemu-Version als Objektdatei auf dem Gerät. Erste Übersetzung einmalig (Größenordnung Minuten bei rund 24.000 Funktionen), danach Laden in Sekunden. Positionsunabhängiger Code, Laufzeitadressen über den Kontext statt als Konstanten.

## Abhängigkeit
LLVM als Bibliothek für Android arm64, gebaut aus den offiziellen Quellen (`llvm-project-*.src.tar.xz`, rund 140 MB Download, Build 1–2 Stunden). APK wird 30–50 MB größer. Nur die Targets AArch64 bauen.

## Stufen
1. LLVM für Android bauen, in Cidemu einbinden, eine einzelne Funktion IML→LLVM→Maschinencode übersetzen und ausführen.
2. Alle IML-Operationen, Funktionen einzeln (ohne Aufruf-Modell), Vergleich gegen den JIT: Korrektheit (LCU läuft), Geschwindigkeit pro Funktion.
3. Aufrufe als echte Aufrufe, Inlining, Mischbetrieb mit dem JIT.
4. Vorab-Übersetzung beim ersten Start mit Fortschrittsanzeige, Cache auf dem Gerät.
5. Messung: Last und Takt von Kern 1/2, Leistung, Temperatur, Übersetzungs-Hänger.

## Risiken
- Korrektheit der Float-Semantik (Single-Rundung, Paired Singles, FMA) muss exakt dem JIT entsprechen.
- Selbstmodifizierender oder nachgeladener Code (RPL-Module) muss den Cache invalidieren.
- Speicherbedarf von LLVM während der Vorab-Übersetzung (genug RAM vorhanden).
