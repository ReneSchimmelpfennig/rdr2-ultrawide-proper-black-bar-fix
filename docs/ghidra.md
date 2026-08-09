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

## Ergebnis der Differenzsuche (2026-08-09)

Gescannt wurde `.data`, 35,6 MB. Pass 1 im Gameplay fand 3860 Floats im
Gradfenster und 11395 im Bogenmaßfenster. Nach Cutscene-Vergleich und
Folgeframe blieben **je zwei** Kandidaten übrig, die sich pro Frame ändern:

| Offset | Gameplay | Cutscene | +1 Frame | Fenster |
|---|---|---|---|---|
| `+0x39B06E4` | **45.000** | 48.840 | 26.991 | Grad |
| `+0x3AE24B8` | **45.000** | 48.840 | 26.991 | Grad |
| `+0x3A11250` | 1.000 | 0.598 | 0.567 | Bogenmaß |
| `+0x3A11254` | 1.000 | 0.801 | 0.820 | Bogenmaß |

Die beiden Gradkandidaten tragen identische Werte, sind also zwei Kopien
desselben Wertes. Exakt `45.000` im Gameplay und Änderung in jedem Cutscene-
Frame — das Profil einer Kamera-FOV.

Die beiden Bogenmaßkandidaten liegen direkt nebeneinander und stehen im Gameplay
beide auf `1.0`. Das sieht nach einem Skalierungspaar (x, y) aus, nicht nach
einem Winkel. Vorerst zurückgestellt.

### Woher die Gradkandidaten kommen

`+0x3AE24B8` wird in einer Per-Frame-Funktion so gefüllt:

```c
DAT_7ff678b824b8 = *(float *)(index * 0x690 + 0x7ff678f61050);
_DAT_7ff679fdf270 = DAT_7ff678b824b8;   // viermal hintereinander -> XMM-Broadcast
```

Also ein **Array von View-Strukturen mit Schrittweite `0x690`**, Basis um
`+0x3EC0B00`. Der Wert wird viermal nebeneinander abgelegt, wie eine
Shader-Konstante. `+0x3AE24B8` ist damit nur eine Kopie, nicht die Quelle.

`+0x39B06E4` wird in einer anderen Per-Frame-Funktion aus einem Getter
(`+0x173ED4`, `return DAT_7ff678f40be0`) geschrieben, direkt gefolgt von einem
`fabs(neu - alt) < eps`-Vergleich. Das ist eine Änderungserkennung, der Wert
selbst ist wieder nur ein Cache.

### Warum die Quelle nicht in der Kandidatenliste steht

`+0x3EA0BE0` liegt im gescannten Bereich, taucht aber nicht auf. Passt zum
Befund: die Suche vergleicht **feste Adressen** über die Zeit. Ein Wert, der
durch ein Array wandert oder indirekt beschrieben wird, fällt durch dieses
Raster — die festen Kopien dagegen nicht. Genau die haben wir gefunden.

Xrefs auf `+0x3EA0BE0` zeigen nur Leser, keinen Schreiber: der Wert wird über
eine berechnete Adresse gesetzt, die Ghidra statisch nicht auflöst.

### Offen: der Nachweis fehlt noch

Dass `45.000` eine FOV in Grad ist, ist **plausibel, aber nicht bewiesen**.
Belegt ist nur: der Wert steht im Gameplay konstant auf 45, ändert sich in
Cutscenes jeden Frame, und wird als Shader-Konstante breitgestellt. Der
eigentliche Beweis wäre, ihn zu verändern und die Bildwirkung zu sehen.

## Nächster Schritt: den Kandidaten beweisen

Alle bisherigen Indizien sind Korrelationen. Der Beweis ist eine Intervention:
den Wert verändern und sehen, ob das Bild reagiert. Das ist ohnehin nötig, denn
die FOV muss am Ende geschrieben werden.

Vorgehen, gestaffelt vom Harmlosen zum Wirksamen:

1. **Beobachten.** Die vier Kandidaten plus den Getter-Wert `+0x3EA0BE0` und das
   Letterbox-Gewicht in einer Zeile pro Frame protokollieren. Zeigt, welche
   Werte miteinander laufen und ob `+0x3EA0BE0` derselbe Wert ist.
2. **FOV-Regler bewegen.** Ändert sich `45.000` mit der Einstellung im
   Spielmenü, ist die Bedeutung geklärt, ohne irgendetwas zu schreiben.
3. **Schreiben.** Einen festen Wert in die Kopie schreiben und schauen, ob das
   Bild folgt. Tut es das nicht, ist die Kopie tot und nur die Quelle im
   View-Array zählt — dann dort ansetzen.

Erst wenn das steht, ist die Schreibstelle im Sinne des Designs gefunden und die
Korrektur aus `framing.h` kann angewendet werden.
