# arply – Design Language

Stand: 2026-11-23. Abgeleitet aus einem Screenshot von arply.de (Mobilansicht,
nur ca. 200 px breit). Farben sind gemessen, Schriften und Maße nach Augenmaß
geschätzt – mit dem echten CSS von arply.de abgleichen, sobald verfügbar
(markiert mit **[prüfen]**).

Umgesetzt in: `calibration-prototype.html` (dieser Ordner) und im Mock der
Übersichtsseite (`arply.local`).

## Grundhaltung

- Ruhig, hell, technisch-präzise. Viel Luft, wenige Elemente pro Abschnitt.
- Eine einzige Akzentfarbe (Indigo); alles andere ist Grau-Blau auf fast Weiß.
- Monospace als zweite Stimme: für Überzeilen, Pins, Befehle, Nummern, Adressen.
- Hardware als freigestellte Fotos auf neutralem Grund, keine Stimmungsbilder.

## Logo

- Wortmarke **„arply“**, durchgehend klein geschrieben, ohne Symbol.
- Dünne, geometrische Monoline-Schrift mit gerundet-eckigen Formen
  (einstöckiges „a“), großzügige Laufweite. Farbe: fast Schwarz.
- Datei: `https://arply.de/assets/arply-logo.png` (im Repo:
  `docs/arply-logo.jpg`). **Nie nachbauen**, immer die Originaldatei
  verwenden (besser als SVG anlegen). Bis die Datei vorliegt: Platzhalter
  (derzeit Comfortaa) klar als solchen kennzeichnen.
- Im Header links; im Footer klein, gefolgt vom Claim
  „An unofficial, open AirPlay receiver.“
- Im Fließtext heißt das Produkt immer klein „arply“.

## Farben

| Rolle | Hell | Dunkel | Verwendung |
|---|---|---|---|
| Hintergrund | `#f7f8fb` | `#0e1016` | Seitenhintergrund, auch `theme-color` |
| Fläche | `#ffffff` | `#161922` | Karten, Hero-Panel, Eingabefelder |
| Linie | `#eceef4` | `#262a36` | Kartenränder, Trennlinien (sehr zart) |
| Button-Rand | `#e3e5ee` | `#30354a` | Rand sekundärer Pillen-Buttons |
| Text stark | `#0b0d14` | `#eef0f6` | Überschriften |
| Text | `#4f556a` | `#a9aec0` | Fließtext |
| Text leise | `#6b6f82` | `#8e93a6` | Hilfstexte, Footer |
| Akzent | `#4e54dc` | `#8b90f0` | Primärbuttons, Links, Überzeilen, Nummern |
| Akzent hell | `#e9eafb` | `#23264a` | Nummernkreise, Code-Chips, Rollen-Labels |
| Akzent-Text auf hell | `#2f34a8` | `#c3c6fa` | Text auf Akzent hell |
| Akzent deaktiviert | `#9ea1ec` | – | deaktivierte Buttons („Coming soon“) |
| Hinweis Fläche | `#f9f3e8` | `#2d2618` | Warn-/Hinweisbox |
| Hinweis Text | `#6b4a10` | `#e8c98e` | Text in der Hinweisbox |

Hinweise:
- Die dunklen Werte gibt es auf der Website nicht; sie sind für Geräteseiten
  abgeleitet.
- Auf der Website ist der Footer-Kleintext heller (`#8f91a6`); das liegt unter
  4,5:1 Kontrast. In eigenen Oberflächen `#6b6f82` verwenden.
- Statusfarben (nicht auf der Website): online `#1f7a4d`, Fehler/offline
  `#a1281c` auf `#fdecea`, sparsam und immer mit Text.

## Typografie

| Rolle | Schrift | Größe (mobil / Desktop) | Gewicht | Sonstiges |
|---|---|---|---|---|
| Hero | Sans | 34–40 / 48–56 px | 700 | Laufweite −0,02 em |
| H2 Abschnitt | Sans | 26–28 / 36 px | 700 | Laufweite −0,01 em, Zeilenhöhe 1,15 |
| H3 Karte/Schritt | Sans | 17–18 px | 600 | |
| Fließtext | Sans | 16 / 17 px | 400 | Zeilenhöhe 1,55–1,6 |
| Klein | Sans | 13–14 px | 400 | Hilfstexte, Footer |
| Überzeile | Mono | 11–12 px | 500 | GROSSBUCHSTABEN, Laufweite 0,12 em, Akzentfarbe |
| Code/Befehl/Adresse | Mono | 0,9 em | 500 | Code auf `#e9eafb`, Radius 6 px |

- Sans: wirkt wie **Inter** **[prüfen]**. Fallback `system-ui, sans-serif`.
- Mono: **JetBrains Mono** in den Geräteseiten **[prüfen]**.
  Fallback `ui-monospace, monospace`.
- Jeder Abschnitt beginnt mit einer Überzeile (z. B. HARDWARE, QUICKSTART,
  STEP 1, ROOM), dann Überschrift, dann ein Satz Einleitung.

## Abstände, Formen, Raster

- Mobil: Seitenrand 20–24 px, Abstand zwischen Abschnitten ca. 72–96 px auf
  der Website, 16–20 px zwischen Karten in Geräteseiten.
- Karten: weiß, 1 px Rand `#eceef4`, Radius 16 px, Innenabstand 20–24 px,
  kein Schatten. Hero sitzt auf der Website in einem großen Panel (Radius ca. 24 px).
- Buttons: Pillenform (Radius 999 px), Höhe 44–48 px, Text 15 px / 600.
- Einspaltig auf dem Handy; auf dem Desktop Karten im Raster
  (2–4 Spalten), Inhaltsbreite ca. 1100–1200 px **[prüfen]**.

## Komponenten

- **Header:** Logo links, rechts ein Pillen-Button mit Rand oder ein Rücklink;
  darunter eine zarte Linie.
- **Button primär:** Akzentfläche, weißer Text, Pille. Eine Primäraktion pro Karte.
- **Button sekundär:** weiße Fläche, 1 px Rand `#e3e5ee`, dunkler Text, Pille.
- **Button deaktiviert:** `#9ea1ec`, weißer Text.
- **Karte:** Überzeile, Titel, ein bis zwei Sätze Text, darunter Buttons.
- **Schritte:** Nummernkreis (28 px, `#e9eafb`, Zahl in Mono/Akzent) bzw.
  Überzeile „STEP 1…5“; Schritte durch feine Linien getrennt.
- **Hinweisbox:** Fläche `#f9f3e8`, Radius 16 px, fetter erster Satz.
- **Chips/Labels:** Status als Pille (13 px); Rollen (LEFT, BASS, HUB) als
  kleine Mono-Labels in Großbuchstaben, Radius 6 px.
- **Formular:** Eingabefeld als weiße Pille mit zartem Rand, daneben der
  Primärbutton; darunter ein kleiner Satz in leisem Text.
- **Footer:** zentriert – Logo + Claim, eine Reihe Links, ggf. rechtlicher
  Hinweis klein.
- **Links:** im Text unterstrichen; Akzentfarbe für Hervorhebungen.
- **Pinout/Technik:** Monospace-Spalten, Signalnamen in Akzentfarbe.

## Diagramme (Kalibrierung)

- Messkurve grau-blau `#9aa0b4` (Einzelmessungen blasser), Ergebnis dunkel
  `#0b0d14` und am kräftigsten, EQ in Akzent, Ziel gestrichelt `#6b6f82`.
- Korrekturbereich als Fläche `#eef0fd`. Achsenbeschriftung 12 px Sans,
  Messwerte im Readout in Mono.

## Sprache und Ton

- Kurz, direkt, freundlich, für Maker und Bastler. Keine Werbesprache.
- Überschriften als kurze Aussage mit Punkt: „Give arply a home.“,
  „Tune arply to your room.“, „Your rooms.“
- Website primär Englisch, mit deutscher Version.
- **Geräteoberflächen (Übersicht, Kalibrierung, Einstellungen) auf Englisch.**
  Zahlen im englischen Format (Punkt als Dezimaltrenner).
- Ehrlich über Grenzen (z. B. „Not a certified product.“).

## Offene Punkte

- [ ] Echtes CSS von arply.de auslesen (Schriften, Größen, Radien, Maxbreite).
- [ ] Logo als SVG ablegen, hier verlinken und den Platzhalter ersetzen.
- [ ] Deutsche Übersetzung der Geräteoberflächen später als Sprachschalter.
