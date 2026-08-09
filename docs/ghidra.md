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

## BEWIESEN: die FOV ist gefunden (2026-08-09)

Kein FOV-Regler im Spielmenü, deshalb der Test mit dem **Fernglas** — kontinuierlicher
Zoom, also ein durchgehender FOV-Verlauf statt diskreter Stufen. Das Ergebnis ist
eindeutig:

| Zustand | Wert |
|---|---|
| Normales Gameplay | **51,282°** (89 von 225 Messzeilen) |
| Leicht verändert (Bewegung, Reiten) | 52,183° |
| Fernglas angesetzt | 22,620° |
| Fernglas voll herangezoomt | **8,578°** |
| Maximum über den ganzen Lauf | 63,589° |

Ein Wert, der beim Heranzoomen auf ein Sechstel fällt, ist ein Blickwinkel.
Damit ist die Frage beantwortet, ohne dass etwas geschrieben werden musste.

### Die Rangfolge der drei Adressen

`+0x3EA0BE0` ist der **Master**. Die beiden Kopien folgen ihm; in 10 von 225
Zeilen hinken sie genau einen Frame hinterher, sonst sind alle drei identisch.
Beim Start stand der Master noch auf `0.0`, während die Kopien schon `45.0`
trugen — die 45 war also ein Initialwert aus dem Menü, nicht die Gameplay-FOV.

| Offset | Rolle |
|---|---|
| `+0x3EA0BE0` | **Master**, von `GetFov()` (`+0x173ED4`) zurückgegeben |
| `+0x39B06E4` | Kopie für die Änderungserkennung |
| `+0x3AE24B8` | Kopie aus dem View-Array, als Shader-Konstante breitgestellt |

### Es ist die vertikale FOV

Nicht direkt gemessen, aber die Gegenrechnung lässt nur eine Deutung zu:

| Annahme | Folge bei 3440x1440 |
|---|---|
| 51,282° ist **vertikal** | hFOV = 97,8° — plausibel |
| 51,282° ist horizontal | vFOV = 22,7° — unmöglich eng |

Das deckt sich mit der Designnotiz: RDR2 ist Hor+, die vFOV bleibt konstant und
die hFOV wächst mit dem Seitenverhältnis. Genau diese Größe braucht
`framing::corrected_vfov_deg()`, und zwar in **Grad** — die Funktion nimmt Grad,
passt also direkt.

### Erledigt: die Bogenmaß-Kandidaten

`+0x3A11250` und `+0x3A11254` bewegen sich unabhängig von der FOV. Ihr Betrag
`sqrt(x²+y²)` schwankt zwischen 0,35 und 1,0 — ein Richtungsvektor ist
plausibel, aber nicht belegt. Für den Fix irrelevant, nicht weiter verfolgt.

## Nächster Schritt: die Schreibstelle

Der **Wert** ist gefunden, die **Schreibstelle** noch nicht. Ghidra sieht auf
`+0x3EA0BE0` nur zwei Leser und keinen Schreiber — geschrieben wird über eine
berechnete Adresse, die statisch nicht auflösbar ist.

Zu klären, in dieser Reihenfolge:

1. Den zweiten Leser (`+0xF98FE8`) dekompilieren. Der Getter `+0x173ED4` ist der
   eine, dieser der andere — zusammen decken sie ab, wer den Master überhaupt
   benutzt.
2. Das View-Array aufklären: `*(index * 0x690 + 0x7ff678f61050)`. Wer dort
   hineinschreibt, committet die Kamera-FOV pro Frame.
3. Danach die Entscheidung, wo der Hook sitzt. Kandidaten: der Getter (klein und
   sauber zu hooken, deckt aber nur einen der beiden Leser ab) oder die
   Schreibstelle selbst (deckt alles ab, ist aber ein größerer Eingriff).

Erst wenn das steht, greift `framing::corrected_vfov_deg()` mit dem
Letterbox-Gewicht als Blendfaktor — beides liegt fertig vor.
