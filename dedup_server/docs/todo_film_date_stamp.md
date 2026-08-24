# STAMP-1 — Film-camera date-stamp treatment for the day date/time label

Status: implemented 2026-08-24 (Italic v0.46 bundled + registered,
DateStampLabel reworked per §3, viewer size override dropped; PostScript
name and ghost/lit width identity verified programmatically). Remaining
before Done: the §5 visual pass / opacity tuning by eye.
Scope: `swift_ui/` only. Applies to **both** consumers of the shared
`DateStampLabel` — the VU day strips and the VIEWER tab's day separators
(decision: restyle the shared component once, keep the surfaces consistent).

## 1. Motivation

The VU tab prints "YYYY-MM-DD  HH:MM" above each day's film strip in DSEG7
Classic amber. It reads as a UI label. It should read as the burned-in
quartz date-back print on photos from an old film camera: smaller, dimmer,
slanted, with a phosphor-bloom glow — and the faint unlit "ghost" segments
behind the lit digits that real LED date-backs show.

## 2. Current state (what changes)

- `swift_ui/Sources/swift_ui/Theme.swift:30` — `DateStampLabel`: DSEG7
  Classic (upright Regular), default `size: 11`, `Theme.dateStampAmber`
  (`#FFB000`) at full opacity, a single shadow (amber 0.6, radius 3).
- Consumers: `DayGroupCard.swift:92` (VU, default size 11),
  `ViewerView.swift:387` (viewer day separator, `size: 10`).
- Font registration: `VurioApp.swift:13` registers
  `Fonts/DSEG7Classic-Regular.ttf` via `CTFontManagerRegisterFontsForURL`.
  License: `Fonts/DSEG-LICENSE.txt` (SIL OFL) — already bundled.

## 3. Design

All numbers below are calibrated starting points — tune by eye in §5, but
stay within the stated intent (smaller/dimmer than today, glow stronger
than today).

### 3.1 True italic font (decision: bundle, not skew)

Bundle **DSEG7 Classic Italic** — the DSEG family (same upstream release
as the bundled Regular, https://github.com/keshikan/DSEG, OFL) ships a
true italic with slanted segments, exactly the look of real quartz
date-backs. Synthetic shear on the Regular was rejected: blurrier at 9 pt
and the segment caps stay upright.

- Add `Fonts/DSEG7Classic-Italic.ttf` next to the Regular (grab it from
  the same DSEG release the Regular came from; keep versions matched).
- Register it in `VurioApp.swift` beside the existing call.
- The `.font(.custom(...))` name must match the face's PostScript name —
  expected `"DSEG7Classic-Italic"` (family "DSEG7 Classic" + italic trait
  is *not* reliably resolvable through `.custom`). Verify after
  registration (e.g. dump `NSFontManager.shared.availableFonts` once in
  debug) and use whatever name actually registered.

### 3.2 Size — smaller

- `DateStampLabel` default `size: 11` → **9**.
- `ViewerView.swift:387` passes `size: 10` explicitly → drop the argument
  and take the new default 9, so both surfaces match.

### 3.3 Color — dimmer

- Lit digits: `Theme.dateStampAmber.opacity(0.72)` (was 1.0). Keep the
  `#FFB000` hue — dimming via opacity lets the dark strip background
  warm the tone the way underexposed film does; do not darken the hex.

### 3.4 Glow — stronger, two layers

Replace the single shadow with layered halation (both on the lit layer):

```swift
.shadow(color: Theme.dateStampAmber.opacity(0.85), radius: 1.5) // tight core bloom
.shadow(color: Theme.dateStampAmber.opacity(0.45), radius: 6)   // wide soft halo
```

Intent: the tight shadow fattens the thin 9 pt segments back to a lit-LED
weight; the wide one is the halation into the emulsion. The glow layers
compensate for §3.3's dimming — the stamp should read *softer*, not
*fainter*, than today.

### 3.5 Ghost segments (decision: yes)

Real date-backs faintly show every unlit segment — a ghost "8888" behind
the lit digits. DSEG7 renders every character in an identical fixed cell,
so a ZStack of two `Text` views in the same font/size aligns perfectly,
cell for cell:

- Ghost string: the lit text with **every ASCII digit replaced by `8`**
  (all seven segments on). Non-digit cells (`-`, `:`, spaces) pass
  through unchanged — DSEG draws them in their own cells, so widths stay
  identical. (`8` is the all-on digit; do not use `~`-style specials.)
- Ghost layer: same font, `Theme.dateStampAmber.opacity(0.08)`, **no
  shadow**, rendered *behind* the lit layer (`ZStack` bottom).
- Implementation stays inside `DateStampLabel` — callers are untouched:

```swift
var body: some View {
    ZStack(alignment: .leading) {
        Text(ghostText).font(stampFont).foregroundColor(amber.opacity(0.08))
        Text(text).font(stampFont).foregroundColor(amber.opacity(0.72))
            .shadow(color: amber.opacity(0.85), radius: 1.5)
            .shadow(color: amber.opacity(0.45), radius: 6)
    }
}
private var ghostText: String {
    String(text.map { $0.isASCII && $0.isNumber ? "8" : $0 })
}
```

## 4. Out of scope

- The date-stamp *text format* (`dateStampText` in `DayGroupCard.swift:82`)
  is unchanged — including the double-space separator workaround.
- No per-photo stamp overlays on thumbnails; this is only the day-strip /
  day-separator label.
- Light-mode variants: the app is dark-theme only today.

## 5. Manual verification

1. VU tab: stamp is visibly smaller and dimmer than before, slanted,
   with a soft amber bloom; the faint ghost "8888" reads *only* on
   close inspection — at arm's length it should disappear. If it's
   noticeable at a glance, halve the ghost opacity.
2. Ghost and lit digits align exactly (no double-vision offset) at both
   1x and 2x (Retina) — this proves the two layers use the same font
   face; a mismatch means the italic fell back to another font.
3. VIEWER tab timeline: day separators show the identical treatment.
4. `1`-heavy dates (e.g. 2011-11-11) still read clearly against their
   ghost — thin digits over an "8" ghost are the worst case.
5. Cold launch: no font-registration warning in the console; the stamp
   never renders in the system font (fallback would render *wider* —
   instantly visible).

## 6. Implementation order

1. Fetch + bundle `DSEG7Classic-Italic.ttf`, register, verify the
   PostScript name (§3.1).
2. Rework `DateStampLabel` (§3.2–§3.5); drop the viewer's explicit size.
3. §5 visual pass; tune opacities/radii within the stated intent.
