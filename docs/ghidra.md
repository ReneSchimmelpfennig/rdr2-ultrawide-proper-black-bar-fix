# Was im entpackten Image steht

Analyse des validierten Dumps (siehe [packing.md](packing.md)) in Ghidra,
2026-08-09, RDR2.exe 1.0.1491.50.

Alle Adressen als **Moduloffset**. Im Dump liegt `ImageBase` bei
`0x7FF6750A0000`; Ghidra-Adresse = Basis + Offset.

## Gefundene Funktionen und Globals

| Offset | Was |
|---|---|
| `+0x3200E8` | `LetterboxUpdate()` — rechnet Gewicht und Balken, pro Frame |
| `+0x320545` | darin: der Store `mov byte [rip+X], 0FFh` (unser AOB-Anker) |
| `+0x174230` | `GetLetterboxWeight()` — `return weight` |
| `+0x173964` | `GetAspectRatio()` — `return g_aspect` |
| `+0x19FBCC` | `GetViewportAspect()` — Breite/Höhe aus dem Viewport, virtueller Call |
| `+0x1A1358` | `BlendAspectToCutscene(obj, aspect)` — Lerp mit dem Letterbox-Gewicht |
| `+0x39751AC` | `g_letterboxWeight` (float) |
| `+0x39751B4` | das Anker-Byte, konstant `0xFF` |
| `+0x395B458` | `g_aspect` (float) = **2.3888888** im Dump = 3440/1440 |

## Die Letterbox-Rechnung, verifiziert

`LetterboxUpdate()` endet mit exakt dem, was gemessen wurde:

```c
weight = max(kanal_a, kanal_b);              // +0x39751AC
bar235   = (1.0 - (16/9)/aspect_235) * 0.5 * weight;
barScreen= (1.0 - (16/9)/GetAspectRatio()) * 0.5 * weight;
g_anchor = 0xff;                             // unbedingt, jeden Frame
```

Konstanten aus `.rdata`, direkt aus dem Dump gelesen:

| Adresse | Wert |
|---|---|
| `+0x330C7A8` | `1.0` |
| `+0x3311378` | `1.7777778` = **16/9** |
| `+0x330F3D4` | `0.5` |
| `+0x330F388` | `1e-06` (Epsilon) |

Damit ist `(1 - k)/2 = 0.127907` nicht mehr Interpretation, sondern nachgelesen.
Das Anker-Byte wird als letzte Anweisung unbedingt auf `0xFF` gesetzt — deshalb
war es in 293 gemessenen Frames konstant. Kein Messfehler, so ist es gebaut.

## Negativergebnis: das Gewicht führt nicht zur FOV

`GetLetterboxWeight()` hat genau **drei** Aufrufer, und keiner davon setzt eine
FOV:

1. `+0x1A1358` — Lerp eines **Seitenverhältnisses** gegen den Cutscene-Wert.
   Die Basis dieses Lerps kommt aus `GetViewportAspect()`, ist also Breite/Höhe,
   keine FOV.
2. `+0x6F1910` (`*param_1 = weight`) — schreibt das Gewicht in einen Puffer,
   sieht nach Shader-Konstante oder Script-Native aus.
3. ein Sprung ohne zugeordnete Funktion.

Der einzige Aufrufer von (1) vergleicht zwei geblendete Aspects und setzt daraus
ein Bool — kein Kameracode.

**Konsequenz:** Das Letterbox-Gewicht speist die Balken und die
Aspect-Interpolation, nicht die Kamera. Die FOV-Schreibstelle muss über einen
anderen Weg gefunden werden; der Pfad „Xrefs auf das Gewicht → Kamera" ist
ausgeschöpft.

## Verworfen: Suche über die Deg→Rad-Konstante

`0.017453292` steht neunmal in `.rdata` — fünfmal davon als Vierergruppe, also
als XMM-Vektorkonstante für SIMD. Der interessanteste Skalar liegt bei
`+0x3311368`, direkt neben `0.999999` (`+0x331136C`) und `16/9` (`+0x3311378`),
die der Letterbox-Code benutzt; gleicher Konstantenpool, vermutlich dieselbe
Übersetzungseinheit.

**Trotzdem unbrauchbar:** Die Xrefs sind bei 40 gekappt und offensichtlich
weit darüber. Eine Engine rechnet an hunderten Stellen Grad in Bogenmaß um.
Das ist kein Filter. Die `rad2deg`-Konstante (`+0x330F200 ff.`) verhält sich
genauso.

Nicht nochmal versuchen.

## Verfolgt: die Objektspur

| Offset | Was |
|---|---|
| `+0x3EA04C8` | `g_cinematicMgr` — Singleton-Pointer, `GetCinematicMgr()` gibt ihn zurück |
| `+0x440 37C` | `GetCutsceneAspect(mgr)` = `*(float*)(*(mgr+0x568)+0x80)` |

`GetCutsceneAspect()` hat zwei Aufrufer, `GetAspectRatio()` fünfzehn. Alle
untersuchten Pfade enden bei **Seitenverhältnissen**, nirgends bei einer FOV.
Das Feld `+0x80` des Unterobjekts bei `+0x568` ist ein Aspect, keine Brennweite.

Damit ist auch dieser Weg vorerst erschöpft: Der gesamte Letterbox- und
Cinematic-Zweig rechnet mit Aspects und Balkenhöhen, die Kamera-FOV wird
woanders gesetzt.

## Nächster Ansatz: Differenzsuche aus dem Plugin

Der deterministische Weg, ohne Ratearbeit. Das Plugin kann das, wofür sonst
Cheat Engine benutzt wird — und zwar automatisiert und mit dem Letterbox-Gewicht
als exaktem Zeitgeber:

1. Abzug des `.data`-Bereichs, während `weight == 0` (Gameplay).
2. Zweiter Abzug, während `weight == 1` (Cutscene steht).
3. Diffen und filtern: 4-Byte-Floats, die sich geändert haben und in einem
   plausiblen FOV-Bereich liegen — 10..90 wenn in Grad, 0.17..1.6 wenn in
   Bogenmaß.
4. Dritter Abzug einen Frame später, noch in der Cutscene: was sich **jeden
   Frame** ändert, ist Kamerazustand, was konstant bleibt, ist Konfiguration.

Das Ergebnis ist eine Kandidatenliste von Moduloffsets. Die gehen zurück nach
Ghidra: Xrefs auf so einen Offset zeigen direkt auf die Schreibstelle — und die
ist genau das, was gesucht wird.
