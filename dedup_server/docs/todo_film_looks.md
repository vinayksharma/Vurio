# LOOK-1..6 — Film looks built from generic tools, driven by the scene

Designed 2026-11-23. Goal: a look such as CineStill 800T is a **recipe made of
ordinary tools**, never a dedicated filter, and a recipe can adapt to the photo
it is applied to (grain by scene brightness, a lane that only runs at night).
The reference look is 800T: tungsten balance, teal shadows, soft shoulder,
visible grain, and red halation around light sources.

Everything below follows the tool chain's rules (one server engine, descriptor
driven UI, gamma sRGB frames in [0,1], see git history for the CHAIN spec).

## 1. The reference pipeline

| # | Stage | Node |
|---|---|---|
| 1 | Halation | split, one or two lanes, full mask, merge `linear_add`: `luma_key` → `blur` → `exposure` → `color_filter` (LOOK-1) |
| 2 | Balance toward tungsten | `white_balance` |
| 3 | Layer crosstalk | `channel_mixer` (LOOK-2) |
| 4 | Film curve, lifted blacks | `curves` |
| 5 | Teal shadows, warm highlights | `color_grade` (LOOK-2) |
| 6 | Trims | `hsl`, `saturation` |
| 7 | Grain (already weighted to the shadows) | `grain` |
| 8 | Vignette | `vignette` |

Halation comes first so that it keys on what the camera clipped, before the
balance moves any channel. Its key is the brightest channel at 0.95: only
clipped pixels are sources, a saturated light (a red neon) counts, and a blue
sky that is merely bright does not. The lane adds in linear light, so the glow
that is obvious over a night sky is faint over a daylight one, and a clipped
area cannot get brighter than it is. `exposure` after the blur stands in for
the energy the clipped source really had.

What the fixed recipe cannot know is left for LOOK-5: sensor noise wants
`denoise` before the grain at high ISO, and the tungsten shift and grain
amount want to follow the scene's brightness.

## 2. Phases

### LOOK-1 — Light tools (implemented 2026-11-23)

- [x] `blur` (layer): Gaussian, `radius` as a fraction of the short side,
  `linear` flag (default on) blurs in linear light.
- [x] `luma_key` (layer): keeps one end of the tonal range and blacks out the
  rest. `keep` highlights|shadows, `key` brightest channel|luminance,
  `threshold`, `softness`. Active at defaults.
- [x] Blend modes `add` (gamma, Photoshop's Linear Dodge) and `linear_add`
  (adds in linear light). Eighteen modes.
- A lane mask cannot stand in for `luma_key`: the mask is applied at the merge
  and would clip the glow back onto the highlights.

### LOOK-2 — Colour tools (implemented 2026-11-23)

- [x] `channel_mixer` (layer): 3×3 matrix, one group per output channel.
- [x] `color_grade` (layer): hue, amount and luminance for shadows, midtones
  and highlights, plus `balance`.

### LOOK-3 — The recipe (implemented 2026-11-23)

- [x] Starter recipe "Tungsten 800" (800T style) seeded on an empty table, and
  shipped as `dedup_server/recipes/tungsten_800.vurio-recipes.json` for import
  into an existing library.

### LOOK-4 — Signals (open)

Per-photo numbers, computed once on the base and cached with the photo:

- From metadata: ISO, shutter, aperture, exposure value
  (`log2(N²/t) − log2(ISO/100)`), flash.
- Measured on a fixed small copy of the base (so preview and export agree):
  mean luminance, clipped-highlight fraction, colour cast, noise level.
- The user-facing signals are blends in plain words: Scene brightness, Light
  colour, Noise, Highlights. Raw EXIF values sit under "Advanced".
- Missing EXIF falls back to the measured signals.

### LOOK-5 — Drivers (open)

- Any numeric param, a node's strength and a lane's merge strength may carry a
  driver: `{signal, points: [[signal value, param value], …]}`, interpolated,
  held outside the end points.
- Resolved at compile, so a driven lane strength of 0 drops the lane for free
  (the compiler already drops strength-0 lanes). This is the conditional
  splitter.
- A value set by hand on a photo wins over the driver for that photo.
- Recipes carry drivers; a chain without them is unchanged.

### LOOK-6 — Client (open)

- Link mark on a slider: choose what it follows, then set the slider. Each
  photo it is set on becomes an example point ("link by example").
- A strip under a linked slider shows the range, the points and this photo.
- Lane "When": Always, In dark scenes, In bright scenes, Custom, plus softness.
- Test strip: the recipe on a few photos across the range.

## 3. Deliberately out of scope

- More than one signal per driver ("dark and warm").
- A linear-light working space for the whole chain; only `blur` and
  `linear_add` leave gamma.
