# LOOK-1..6 — Film looks built from generic tools, driven by the scene

Designed and implemented 2026-11-23. Goal: a look such as CineStill 800T is a **recipe made of
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

Halation comes first so that it keys on what the camera recorded, before the
balance moves any channel. It was reworked 2026-11-23 against how the film
behaves (Dehancer's and FilmBox's descriptions): the first version washed
large white areas pink in ordinary indoor light.

- **Source: red light.** `luma_key` keys on the red channel at 0.95. The glow
  is light reflected back into the red-sensitive layer, so a blue LED or a
  blue sky gives none and a tungsten lamp gives a lot.
- **Points of light, not bright areas.** `luma_key`'s `size_limit` (0.05)
  keeps a lamp or a specular whole and only a quarter of the edge of a large
  bright area: a sky or a white wall at clipping is barely over the exposure,
  a lamp is many stops over, and the clipped pixels alone cannot tell them
  apart. A ridge against a blown sky keeps a thin red fringe, as on the film.
- **Only against darker surroundings.** Each lane's mask is a luminance range
  (full below 0.35, gone by 0.65) measured on the picture entering the split.
  Halos show on the dark side of a contrast edge; the inside of a bright area
  and a bright background are left alone.
- **Orange at the edge, red further out.** The tight lane (radius 0.008) is
  tinted at hue 22, the wide one (0.03, the "aura" of a stock without its
  backing) at hue 4, both fully saturated so blue gets nothing.
- **Stronger the darker the scene.** `exposure` after the blur stands in for
  how far over the exposure the source really was; it is driven by
  `scene_brightness`, +1 EV at night down to −1.5 EV in daylight (wide lane
  +0.5 to −2), where a clipped white is barely over.
- **Linear light.** The merge is `linear_add`, so a clipped area cannot get
  brighter than it is.

What a fixed recipe cannot know is driven (LOOK-5): `denoise` before
everything by the photo's noise, the tungsten shift and the grain amount by
the scene's brightness.

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

- [x] Starter recipe "CineStill 800T", preloaded: starters are offered to
  every library once (`recipes.builtinsSeeded` in user settings remembers the
  names), so it reaches an existing library on the next server start and stays
  deleted if the user removes it.

### LOOK-4 — Signals (server implemented 2026-11-23)

`scene_signals.hpp`. Per-photo numbers, measured once per file and held in
memory by `SignalService` (the preview's base decode, EXIF from
`scanned_files`). Served by `RecipesService.GetSceneSignals`.

- [x] Plain signals in [0,1], always available when the file can be read:
  - `scene_brightness`: with ISO, shutter and aperture,
    `EV100 = log2(N²/t) − log2(ISO/100)` moved by how far the picture's mean
    linear luma sits from mid-grey (± 3 EV), mapped −2 EV → 0, 14 EV → 1.
    Without them, the picture alone: `0.5 + log2(mean/0.18)/8`.
  - `light_colour`: `0.5 + 0.25·log2(mean R / mean B)` in linear light.
  - `noise`: `log2(ISO/100)/7`; without an ISO, the measured sigma over 0.04.
  - `highlights`: the share of pixels with a channel ≥ 250, over 5 %.
- [x] Camera signals (`advanced`), only when the metadata has them: `iso`,
  `shutter`, `aperture`, `exposure_value`.
- Measured on a copy no larger than 256 px, so every render size agrees.
- Not persisted: a server restart measures again on first use.
- [x] A reading taken from a stand-in base is answered but not remembered.
  A RAW whose QHD preview is not cached yet decodes through a quick identity
  render that is about two stops from the real base, so a photo without
  EXIF read 0.16 on first open and 0.43 afterwards. Open point: a recipe
  applied in those first seconds still resolves against the stand-in.

### LOOK-5 — Drivers (implemented 2026-11-23)

`drivers.hpp`.

```
tool node:  "drivers": {"<numeric param id>" | "strength": DRIVER}
lane:       "drivers": {"strength": DRIVER}        (the merge strength)
DRIVER:     {"signal": "<id>", "points": [[signal, value], …]}   1–8 points
```

- [x] **Resolved, not rendered.** `Drivers::resolve` writes each driven value
  into the chain for one photo's signals. The engine, the canonical chain and
  the revision only ever see plain numbers, so nothing about rendering,
  caching or history changed, and a value moved by hand simply stays.
- [x] Resolved when a recipe is applied (`RecipeStore::apply`) and on request
  (`RecipesService.ResolveDrivers`, nothing committed).
- [x] A driven lane strength of 0 makes the lane a no-op the compiler already
  drops: the conditional lane.
- [x] Validated wherever a chain is compiled: known signal, numeric target of
  the node's tool, values within the target's limits.
- [x] A photo without the signal keeps the value the chain has.
- [x] The drivers stay on the node after resolving, and are part of a
  recipe's identity (`contentHash`); a chain without drivers hashes as before.
- [x] "CineStill 800T" follows the scene: grain amount and the tungsten shift
  by `scene_brightness`, a leading `denoise` by `noise` (identity, so
  dropped, on a clean photo).
- [x] A pasted chain is resolved for the photo it lands on: the client asks
  `ResolveDrivers` for the incoming nodes before it merges and commits them
  ("Paste chain", replacing or appending). The photo's own nodes are left
  as they are when appending. A chain whose drivers the server refuses is
  not pasted.

### LOOK-6 — Client (implemented 2026-11-23)

`ChainDrivers.swift` (the model), `DriverViews.swift` (the views),
`TestStrip.swift`.

- [x] The panel keeps `drivers` when it sends a chain back (a node's and a
  lane's both round-trip).
- [x] **Link mark** on every number slider and on a node's Strength: a menu
  of the signals (camera ones apart, the ones this photo lacks disabled)
  and "Stop following". Linking takes the slider's value on this photo as
  the first point.
- [x] **Link by example.** A linked value moved by hand becomes a point at
  this photo's reading (`ChainDocument.reconcileDrivers`, run whenever the
  chain is read for a commit or a copy, against the chain as the server
  last had it). A point within 2 % of the signal's span is the same example
  and takes the new value; a ninth point replaces the nearest. Where the
  photo lacks the signal the driver is dropped instead, so the hand-set
  value is never resolved again elsewhere. Until the readings have arrived
  nothing is touched.
- [x] **Strip** under a linked slider: the signal's range left to right
  with its end names, the points, and this photo as the accent line. The
  height is the span the points cover, so a gentle slope still reads.
- [x] **Lane "When"**: Always, In dark scenes, In bright scenes, Custom.
  The presets are two-point ramps on `scene_brightness` (dark centred on
  0.45, bright on 0.55) with a Softness slider (the ramp's width, 0.02 to
  0.6 of the span); Custom adds the signal, which end is on, and the
  threshold. The lane's strength takes the value the ramp gives on this
  photo at once. Setting the strength by hand where the lane is on scales
  the ramp; where it is off it becomes an example point.
- [x] **Test strip** (recipes menu, enabled when the chain follows
  anything): up to five photos spread across the signal's range, the open
  one always among them, each rendered with the chain resolved for it.
  Chosen from the strip's selection, else from the open photo's 24 nearest
  neighbours. Geometry nodes are left out. Nothing is committed, and the
  render sessions opened for it are released when it closes.
- Drivers live on each photo's chain: examples gathered on one photo reach
  another by copying the chain or saving it as a recipe.

### LOOK-7 — A recipe is one row (implemented 2026-11-23)

`ChainGroups.swift`; `RecipeStore::apply` and `prepared`.

- [x] The server tags the steps of one application with
  `"group": {"id", "name"}` (a fresh id per application, the recipe's
  name). The engine, the canonical chain and the revision never read it; a
  recipe saved from a chain drops it.
- [x] The chain list shows each run of trunk nodes sharing a group as one
  row (book icon, name, step count), selected when the recipe lands. Its
  eye mutes every step, its bin removes them, it drags and moves (⌘↑/⌘↓)
  whole, past the whole of a neighbouring recipe.
- [x] The chevron (or a double-click) shows the steps indented under the
  row; they are ordinary nodes and adjust as before.
- [x] Selected, the row's inspector lists the steps and offers Show/Hide
  steps and Ungroup (the steps become ordinary nodes).
- [x] A tool added with the recipe selected goes after it. A node placed
  between two steps joins the recipe; a step moved past the recipe's edge
  leaves it (one move takes it out, in place); an ordinary node steps over
  a closed recipe and into an open one.
- [x] If something splits a recipe (another recipe dropped inside it) the
  longer run keeps the name. A pasted chain's recipes get fresh ids.
- Chains applied before this have no tag and stay as separate steps.
- Not built: a strength for the whole recipe.

## 3. Deliberately out of scope

- More than one signal per driver ("dark and warm").
- A linear-light working space for the whole chain; only `blur` and
  `linear_add` leave gamma.
