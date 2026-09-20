# PLACE-1..3 — Place-Name Search: Offline Reverse Geocoding + Consented Online Lookup

Status: PLACE-1 and PLACE-2 (plus the §6.1 inferred-place rule) IMPLEMENTED
2026-09-19 on branch `feature/search_improvements`; PLACE-3 (online lookup +
consent) designed, deferred by the user.
Builds on EXIF-3 (`LocationGeocoder`), EXIF-7/QP-1 (location facts are always
bound, preposition-triggered, unresolved names demote to CLIP).

## 1. Problem

"waterfalls in yellowstone" should return the waterfall photos taken inside
Yellowstone. Today that works only because Yellowstone happens to be one of
597 entries in the bundled gazetteer (`geodata/world_locations.geojson`:
198 countries, 50 US states, 101 US/CA national parks, 248 major cities).
It answers one question only — place name → bounding box — and the search
turns that box into a `exif_gps_lat/lon BETWEEN` filter. What it cannot do:

- Towns, non-US parks, landmarks: "in bozeman", "in banff", "at old faithful",
  "lower falls" are all unknown → silently demoted to a CLIP text query,
  which returns visually-similar photos from anywhere.
- Reverse direction: nothing turns a photo's GPS into a name, so the viewer
  cannot show "Yellowstone NP · Wyoming", there is no place to browse by, and
  a name is searchable only if we shipped it in advance.
- Bounding boxes lie: Yellowstone's bbox includes West Yellowstone town and a
  strip of Montana; a city bbox includes its suburbs. Fine for parks and
  states, wrong for "in the city".

Measured on the live library (`data/dedup_server.db`, 2026-09-19):

| metric | value |
|---|---|
| files | 6,378 |
| files with GPS | 1,895 |
| distinct ~1 km cells | 71 |
| distinct ~300 m clusters | 109 |
| distinct ~500 km regions | 7 |

Photos cluster hard. Whatever we look up, we look up per cluster, not per
photo: ~100 lookups covers the whole library.

## 2. Answer to "do we have a location database?"

Partially. We have a forward-only name→bbox table with 597 rows. We do not
have reverse geocoding (GPS→name) at any granularity. Both halves of this
design add one:

1. **Offline first** (no network, no consent): a real gazetteer built from
   free, redistributable datasets, plus an offline reverse geocoder. Covers
   every populated place worldwide (down to unnamed-population hamlets),
   parks, terrain, and named natural features (waterfalls, peaks, lakes). Good enough for "in bozeman", "in
   banff", and, for GeoNames-listed features, "lower falls".
2. **Online second** (opt-in, explicit consent, off by default): fills what
   offline cannot — neighbourhoods, street-level names, points of interest,
   and the provider's own "area of interest" labels ("Yellowstone National
   Park" from a point inside the polygon, not just a bbox). Runs in the
   macOS client, never in the server, only after the user says yes.

## 3. Data sources (all free, usable non-commercially, redistributable)

| dataset | license | use | size (est.) |
|---|---|---|---|
| GeoNames `cities500` | CC BY 4.0 | populated places ≥500 pop, with lat/lon, country, admin1/admin2, population, alt names | ~200k rows, ~20 MB SQLite |
| GeoNames feature subset from `allCountries` | CC BY 4.0 | `L.PRK` parks, `H.FLLS` waterfalls, `T.MT/PK` peaks, `H.LK` lakes, `T.BCH` beaches, `T.GLCR` glaciers, `S.MNMT/HSTS` monuments | rows TBD by script; cap via feature-class list |
| Natural Earth admin-0/admin-1 (1:50m) | public domain | country/state polygons or bboxes; already the source of the current gazetteer | ~3 MB |
| Natural Earth parks & protected areas | public domain | already bundled (US/CA only) | in current file |

Rejected: OpenStreetMap planet extracts (ODbL share-alike on the derived DB
is manageable but the build pipeline is heavy); Who's On First (neighbourhood
detail is exactly what the online layer gives us cheaper); any keyed
commercial API (Google, Mapbox, HERE, OpenCage) — excluded by the
non-commercial requirement and by needing per-user keys.

GeoNames requires attribution: a `geodata/LICENSES.md` plus a line in
Settings › Places ("Place data © GeoNames, CC BY 4.0"). Natural Earth needs
none.

### 3.1 Measured: what fits in a 1 GB offline database (2026-09-19)

Loaded the full GeoNames `allCountries` dump (13,472,134 features, 402 MB
zip) into SQLite with the schema below and measured on disk:

| build | rows | on disk | notes |
|---|---|---|---|
| naive full: TEXT/REAL columns + name index + lat/lon index | 13.47 M | 1,758 MB | over budget |
| compact full: int lat/lon (1e-5°), fcode as small int, clustered `WITHOUT ROWID` by 0.01° cell (no separate spatial index), nocase name index, partial ASCII-name index | 13.47 M | 1,237 MB | over budget by 24 % |
| **curated**: compact minus streams, wells, farms, schools, churches, hotels, cemeteries, generic buildings, roads, undersea, vegetation, historical/abandoned places | **9.08 M** | **732 MB** | **recommended** |

The curated build keeps *every* populated place (5.17 M, including the
4.7 M hamlets with no population figure — that is what makes "nearest
place" meaningful in rural areas), every terrain feature (1.84 M hills,
mountains, peaks, islands, valleys, passes, canyons, beaches, dunes),
lakes/waterfalls/glaciers/springs/geysers/bays (818 k), all admin divisions
(494 k, ADM1–ADM5), parks/reserves/localities (398 k), and landmark spots
(344 k: monuments, historic sites, museums, castles, towers, lighthouses,
bridges, caves, arches, campgrounds, stations, airports) plus trails.

Spot checks: Bozeman, Moab, Banff (CA and GB), Delicate Arch, Angels
Landing, Yellowstone National Park, Old Faithful Geyser, Fairy Falls,
Gibbon Falls, Tower Fall Campground all present with coordinates.
Gotcha found: GeoNames codes US waterfalls as `H.OVF` (overfalls), not
`H.FLLS` — the keep-list must include both.

Remaining ~270 MB of the budget, in priority order:

1. Natural Earth 1:10m admin-0/admin-1 polygons (~25 MB) → exact
   country/state containment instead of nearest-town inference.
2. GeoNames `alternateNamesV2` filtered to English + local-language names
   for the curated rows (~150–250 MB est., not measured) → "Munich" finds
   München, "Florence" finds Firenze.
3. Park polygons (OSM `boundary=national_park` extract, ODbL) → true
   "inside Yellowstone" instead of bbox. Size TBD.

Below 1 GB the database ships with the app; it is a single SQLite file
opened read-only. At this size a k-d tree over 9 M points is ~200 MB of
RAM, so nearest-place lookups use the cell clustering instead: scan the
3×3 (or growing ring of) 0.01° cells around the photo via the primary
key — µs per query, zero RAM beyond SQLite's page cache.

The build is a script, `scripts/build_places_db.py`, that downloads the
GeoNames dumps at *build* time on the developer machine, filters, and emits
`geodata/places.sqlite`. The artifact is committed (or fetched in CI); the
end-user app never downloads gazetteer data. The §3.1 keep-list is the starting point; the budget is 1 GB.

## 4. Storage

New tables (migration alongside the existing ALTER-based ones):

```sql
-- One row per known place, whatever the source.
CREATE TABLE places (
    id           INTEGER PRIMARY KEY,
    name         TEXT NOT NULL,
    name_norm    TEXT NOT NULL,          -- lowercase, diacritics folded
    kind         TEXT NOT NULL,          -- country|region|county|locality|sublocality|park|landmark|water|peak|poi
    lat REAL, lon REAL,
    lat_min REAL, lat_max REAL, lon_min REAL, lon_max REAL,   -- nullable bbox
    country_code TEXT,
    parent_id    INTEGER,                -- locality → region → country
    source       TEXT NOT NULL,          -- geonames|naturalearth|apple|osm
    external_id  TEXT,                   -- geonameid / osm id / apple none
    UNIQUE(source, external_id)
);
CREATE INDEX idx_places_name_norm ON places(name_norm);
CREATE VIRTUAL TABLE places_fts USING fts5(name, alt_names, content='');  -- FTS5 is compiled in (verified)

-- Which places a file is "in". Several roles per file.
CREATE TABLE file_places (
    file_id    INTEGER NOT NULL,
    place_id   INTEGER NOT NULL,
    role       TEXT NOT NULL,            -- country|region|county|locality|sublocality|park|landmark|poi
    distance_m REAL,                     -- 0 for containment, >0 for "nearest"
    source     TEXT NOT NULL,            -- offline|apple|osm
    PRIMARY KEY (file_id, role, place_id)
);
CREATE INDEX idx_file_places_place ON file_places(place_id);

-- Cache of online lookups, keyed by cluster, so a re-import never re-asks
-- and the request budget is bounded by geography, not by photo count.
CREATE TABLE place_lookup_cells (
    cell_key    TEXT PRIMARY KEY,        -- "lat3,lon3" of the cluster centroid rounded to 0.001°
    lat REAL, lon REAL,
    provider    TEXT,                    -- apple|osm
    status      TEXT NOT NULL,           -- pending|resolved|failed|skipped
    resolved_at INTEGER,
    attempts    INTEGER DEFAULT 0,
    payload     TEXT                     -- provider JSON, verbatim, for re-derivation
);
```

`scanned_files` gains two denormalised columns for cheap display:

- `place_label TEXT` — "Lower Falls · Yellowstone NP · Wyoming · US", built
  most-specific-first, at most 3 segments.
- `place_level INTEGER` — 0 no GPS, 1 offline-resolved, 2 online-resolved.

The current GeoJSON gazetteer is imported into `places` (source
`naturalearth`) on first run and `LocationGeocoder` becomes a thin reader over
`places`, so there is one place table, not two.

## 5. Offline reverse geocoder (PLACE-1)

`OfflineReverseGeocoder` (C++, server), loaded once, thread-safe reads:

1. **Nearest populated place**: k-d tree (or a 0.5° grid of buckets; 200k
   points, either is µs per query) over `cities500`. Yields locality,
   admin1 (region), country from the row itself — no polygon test needed. The
   row's `distance_m` is stored; a photo 40 km from the nearest town is still
   "near Bozeman" but the UI can say "near".
2. **Containment**: parks and admin regions with a bbox (Natural Earth) —
   point-in-bbox, later point-in-polygon if we ship polygons.
3. **Nearby named features** within `search.places.landmarkRadiusM`
   (default 1,500 m): waterfalls, peaks, lakes, monuments. Each becomes a
   `landmark` role row. This is what makes "lower falls" findable offline
   when GeoNames has the feature.

Runs in two places, both existing patterns:

- **At scan time**: right after EXIF extraction writes `exif_gps_*`, in the
  same worker — cost is negligible next to ImageMagick.
- **Backfill**: a `MediaBackfill::backfillPlaces` sweep, wired into the
  integrity check as a phase after the RAW EXIF sweep, selecting
  `exif_gps_lat IS NOT NULL AND place_level = 0`. Uses the existing
  `MediaBackfillHook::ProgressFn` so Settings shows "Resolving place
  names… 812 / 1,895".

Video GPS (QuickTime `com.apple.quicktime.location.ISO6709`) is not extracted
today; out of scope, noted for the video program.

## 6. Search integration (PLACE-2)

A location fact (`in X`, `at X`, `near X`, `from X`, per EXIF-7) currently
resolves through `LocationGeocoder::lookup` → bbox → bound filter. New
resolution order in both `LlmFactValidator` and the regex `ExifQueryParser`:

1. **Places that have photos**: `places` joined to `file_places`, matched by
   `name_norm` exact, then FTS prefix/token match. Restricting to places with
   photos resolves ambiguity for free: "paris" means whichever Paris the
   user has been to; if they've been to both, both match (union). Yields a
   place-id set → `file_id IN (SELECT file_id FROM file_places WHERE
   place_id IN (…))`.
2. **Gazetteer bbox** for large regions (countries, states, parks) — kept,
   OR-ed with (1), so "in wyoming" still works for photos whose nearest
   town is across the state line.
3. **Unresolved** → unchanged: demote to CLIP. But the search response now
   also carries `unresolved_places: ["bozeman"]` and `places_pending_online:
   N` (count of GPS clusters not yet online-resolved) so the client can
   offer the consent prompt at the exact moment it would have helped.

### 6.1 Bare place names ("yellowstone waterfalls") — let the LLM decide

Decision (2026-09-19): the LLM structurer, not a preposition regex, decides
whether a bare token is a place. Probed the shipped Qwen2.5-1.5B on 12 bare
queries through the real structurer:

| query | model output |
|---|---|
| yellowstone waterfalls | location=yellowstone, span "in yellowstone" (span invented), clip "yellowstone waterfalls" |
| paris cafe / rome ruins / banff lake reflections | location fact each, plus one hallucinated camera=iphone on "cafe" |
| sunset over the grand canyon | location=grand canyon, plus a bogus date fact on "sunset" |
| bozeman farmers market | location value = whole phrase (unresolvable) |
| phoenix at night | location=phoenix with span "at night" |
| nice sunset | no location (good), bogus aperture=2.8 explore fact |
| turkey dinner | location=turkey (wrong) |
| old faithful erupting | no location; empty camera explore fact |

So the model already does the "rewrite to `waterfalls in yellowstone`"
implicitly — it emits a location fact for bare names. Today those facts
die in the validator because the invented span ("in yellowstone") is not a
substring of the query; that is the only thing enforcing the preposition
rule on the LLM path. The regex fallback parser keeps its preposition rule
(no judgment available there).

Mechanism:

1. **Validator**: accept a location fact whose *value* is grounded in the
   query even when its span is not (span-grounding stays mandatory for every
   other dimension). Mark such facts `inferred=true`.
2. **Resolution gate**: an inferred location must resolve against *places
   that have photos* (§6 step 1) or a gazetteer bbox; otherwise it is dropped
   and the query is unchanged (CLIP still sees the full text). This kills
   "bozeman farmers market" → nothing, and "turkey dinner" unless the user
   has photos in Turkey.
3. **Merge**: identical to today — the resolved place is a **bound** filter
   intersected with the CLIP results, exactly like the preposition form.
   No new algebra case.
4. **Empty-result guard for inferred facts only**: if the bound intersection
   returns zero files, re-run once without the inferred location. A
   preposition form ("in turkey") never falls back — the user was explicit.
5. **Prompt**: add few-shot examples for "yellowstone waterfalls" (place +
   clip), "nice sunset" and "turkey dinner" (no location), and a rule
   "a bare place name is a location only when the rest of the query is a
   scene or subject; food, animals and adjectives are never places". The
   golden matrix gains these as acceptance tests.

The hallucinated side facts seen in the probe (camera=iphone, date=sunset,
aperture=2.8) are already dropped by span/value grounding and stay that
way; they cost nothing.

"waterfalls in yellowstone" end to end: LLM → `{location: yellowstone}` +
`clip: ["waterfalls"]` → file set = photos with a Yellowstone
`park`/`locality` role ∪ bbox hits → intersected with CLIP("waterfalls") →
ranked. With landmark roles populated, a photo tagged "Lower Yellowstone
Falls" also matches a bare "falls" token if we enable the optional rule.

## 7. Online lookup with consent (PLACE-3)

### 7.1 Where it runs

In the **Swift client only**. The server never opens an internet socket
(it links Poco::Net without NetSSL, and keeping it that way is a feature:
"the server is offline" is a one-line privacy statement). The client already
is the user-facing, sandbox-aware process, and macOS ships a free, keyless
reverse geocoder.

### 7.2 Providers

| provider | cost/key | terms that matter | attribution | quality |
|---|---|---|---|---|
| **Apple MapKit** (`MKReverseGeocodingRequest`, macOS 26; `CLGeocoder` fallback on older macOS — verify deprecation status against the SDK during implementation) | free, no key | throttled by Apple; serialise requests, back off on `CLError.network`; Apple's developer terms permit in-app use | none required for non-map display (verify) | excellent: locality, subLocality, administrativeArea, country, `areasOfInterest` (parks, landmarks), `name` (POI) |
| **OSM Nominatim** public instance | free, no key | usage policy: ≤1 req/s, identifying `User-Agent` (`Vurio/<version> (<contact URL>)`), results must be cached, no bulk jobs — ~100 cluster requests is well inside | "© OpenStreetMap contributors" wherever results are shown | good; `zoom=16` gives neighbourhood/POI, JSON `address` hierarchy |

Recommendation: ship **Apple** in PLACE-3, add Nominatim as a second provider
behind the same protocol later (it's a `PlaceProvider` protocol
implementation plus the attribution line). Both are OK for non-commercial
use; Apple is keyless, native, has no attribution burden, and its
`areasOfInterest` is exactly the "which park am I in" signal we want.

### 7.3 Consent model

Config keys (server-owned, so they persist and show in Settings ›
Configuration like everything else):

```
search.places.online:            ask | allowed | denied      (default: ask)
search.places.onlineProvider:    apple | osm                 (default: apple)
search.places.clusterMeters:     300
search.places.landmarkRadiusM:   1500
```

Rules, enforced in code and covered by tests:

- **No request leaves the machine unless `online == allowed`.** The client's
  `PlaceResolver` reads the config on start and on every config change;
  `ask`/`denied` means the resolver object is never instantiated.
- **What is sent**: one coordinate per ~300 m cluster (centroid rounded to
  0.001°, ~110 m). Never file names, never thumbnails, never timestamps,
  never per-photo coordinates. Stated verbatim in the consent dialog.
- **How much**: bounded by clusters (`place_lookup_cells` rows with status
  `pending`), each looked up once, cached forever, retried at most 3 times
  with backoff.
- **Revocable**: turning the toggle off stops the resolver immediately;
  already-resolved names stay (they're the user's data now) with a "Clear
  online place names" button that deletes `file_places WHERE source != 'offline'`
  and resets cells to `pending`, mirroring FACE-8b's "Clear All Faces".

### 7.4 Where the user sees it

1. **Settings › Places** (new section, same visual language as the People
   sections): status line "1,895 photos have GPS · 1,895 named offline ·
   109 areas could be improved online"; toggle "Look up place names online";
   provider picker; the exact "what is sent" text; attribution lines
   (GeoNames always; OSM when selected); progress bar while resolving;
   "Clear online place names".
2. **Inline, at the moment it matters** — the search response carries
   `unresolved_places` / `places_pending_online`, and the VU search bar
   shows a one-line notice under the field:

   > "bozeman" isn't in Vurio's offline map. Let Vurio ask Apple Maps for
   > place names? It sends one approximate location per area (109 areas),
   > never your photos. **Allow** · **Not now** · **Details…**

   *Allow* sets `allowed` and starts the resolver; the search re-runs
   automatically when the pending count hits zero. *Not now* dismisses for
   the session (config stays `ask`). *Details…* opens Settings › Places,
   which also has "Don't ask again" (= `denied`).
3. **First-run**: no dialog. Nothing is sent until asked, so nothing needs
   to be said until the feature would help.

### 7.5 Protocol (gRPC additions)

```
rpc ListPlaceLookupWork(ListPlaceLookupWorkRequest) returns (ListPlaceLookupWorkResponse);
  // → up to N pending cells {cell_key, lat, lon, photo_count}; empty when online != allowed
rpc SubmitPlaceLookup(SubmitPlaceLookupRequest) returns (SubmitPlaceLookupResponse);
  // ← {cell_key, provider, ok, error, places: [{name, kind, role, lat, lon, external_id}]}
```

Server on submit: upsert `places` (source = provider), insert `file_places`
for every file whose cluster is `cell_key`, rebuild `place_label`, set
`place_level = 2`, mark the cell resolved, store the provider payload. The
server also refuses `SubmitPlaceLookup` when `online != allowed` — the
consent check lives on both sides.

Client `PlaceResolver` (actor): polls work every 30 s while the app is open
and `allowed`; serialises provider calls; Apple: one in flight, 1 s spacing,
exponential back-off on throttle; OSM: strict 1 s spacing. Shows progress via
the existing status plumbing.

## 8. UI surfaces beyond search (small, PLACE-1/2)

- Viewer metadata overlay and DayGroupCard get `place_label` next to the date
  stamp. Server already sends per-file EXIF in `DayFile`; add `place_label`
  (next free field number).
- A PLACES tab (browse by country → region → place, like PEOPLE) is a natural
  follow-on and is **not** in this program.

## 9. Testing

- `OfflineReverseGeocoder` unit tests on a 30-row fixture gazetteer:
  nearest-locality, park containment, landmark radius, dateline/poles edge
  cases, GPS (0,0) stays NULL-equivalent (the EXIF-1 lesson).
- Parser/validator tests: "in bozeman" resolves through `file_places`;
  ambiguous "paris" unions both; unresolved still demotes to CLIP and
  populates `unresolved_places`.
- Golden-matrix additions (real Qwen): "waterfalls in yellowstone",
  "at old faithful", "near banff".
- Consent tests: with `ask`/`denied`, `ListPlaceLookupWork` returns empty and
  `SubmitPlaceLookup` is rejected; Swift test that `PlaceResolver` is not
  constructed unless `allowed`; provider protocol tested with a fake.
- e2e fixture pins `search.places.online: denied` (hermeticity, like
  `search.llm.enabled=false`).

## 9.1 Implementation notes (2026-09-19, PLACE-1/2 done)

- `scripts/build_places_db.py --dump-dir DIR` → `geodata/places.sqlite`
  (731 MB, 9,076,265 places, gitignored via `*.sqlite`; rebuild per machine).
  `geodata/LICENSES.md` carries the GeoNames CC BY attribution.
- `OfflineReverseGeocoder` (sqlite3 C API, read-only, cell-range scans):
  live lookups measure 0.1–1 ms. Locality = min over
  `distance / (1 + 1.5·log10(1+population))`; first scan square ±5 cells
  (~5.5 km) so a city a few km off competes with the hamlet next door.
  Region/country come from the NEAREST resolved feature, and when a
  landmark is within 500 m the town must share its state (the Jersey City
  waterfront is nearer Manhattan's point than Jersey City's). GNIS
  "(historical)" names are never landmarks. Park containment uses the
  small bbox gazetteer; on identical boxes the longer name wins
  ("Yellowstone NP").
- Startup sweep: server init counts GPS rows below the resolver version
  and names them on a detached thread (sub-ms per photo) — the feature
  never waits for a manual integrity check. The integrity-check phase stays
  as the explicit re-run path.
- `PlaceResolver` singleton: configured in server init; called from
  ImageProcessor right after the EXIF write (GPS present) and from the
  integrity-check hook, which now chains "Refreshing RAW photo metadata..."
  and "Resolving place names..." phases.
- `PlacesOps`: `places` + `file_places`; `place_label`/`place_level` are on
  the scanned_files CREATE and the ScannedFilesOps migration list because
  the day-view SELECT (both UNION branches) reads `place_label`.
- Search: `ExifQueryConstraints::place_ids` (places with photos, whole-word
  name match) OR-ed with the bbox; `location_inferred` is set by the
  validator when a location fact's span was invented OR carries no location
  preposition; the engine (`runAlgebra`, split out of `searchUncached`)
  retries once without an inferred place when the result is empty. The
  regex parser never infers.
- Live run over a copy of the author's library: 1,894 GPS photos resolved
  in 0.77 s, 0 failures; "photos in Hoboken" → 550 files. Sample
  labels: "Kinryū-zan Sensōji · Asakusa · Tokyo", "near Southwest Harbor ·
  Maine · United States", "McKenna Triangle · Washington Heights · New York".
- Tests: OfflineReverseGeocoderTest (mini gazetteer in the production
  layout), PlacesOpsTest, PlaceResolverTest, MediaBackfillPlacesTest,
  ExifQueryParserPlaceTest, LlmFactValidatorPlaceTest, 7 e2e cases, 3
  golden-matrix cases (real Qwen: "yellowstone waterfalls" → inferred bound
  place + clip "waterfalls"). Gated extras: `OfflineReverseGeocoderLiveTest`
  (needs the built gazetteer) and `PlaceResolverLiveTest`
  (`VURIO_LIVE_DB=<copy of a library>`).
- Not done (by design): videos carry no GPS extraction; files whose GPS
  disappears on re-scan keep stale place rows (never observed in practice).

## 9.2 PLACE-4 — Settings: "waterfront", "beach", "lake" as geo facts (designed + built 2026-09-19)

**Problem.** "new jersey waterfronts" is two facts about *where*, not one
place plus a visual guess. Today the place binds and CLIP decides what a
waterfront looks like, so a rocky shore in Maine can outrank a photo taken
on the Hudson. And "near the waterfront" and "on the waterfront" mean
different distances.

**Design.**

1. *Setting categories* — a curated map in `config/exif_search_keywords.json`
   (`settings`, mirrored by `ExifKeywordConfig::defaults()` under the same
   lockstep test as `file_types`; alphabetical, Poco iteration order):
   category name → GeoNames feature codes. waterfront = bays, coves,
   harbours, inlets, lagoons, sounds, straits, channels, seas, oceans,
   gulfs, beaches, piers, marinas, ports, lakes, reservoirs, ponds,
   anchorages; beach = T.BCH; lake = H.LK/LKS/RSV/PND/LGN; mountain = T.MT/
   MTS/PK/HLL/HLLS/RDGE/PASS/VLC; park = L.PRK/RES*; trail = R.TRL; forest
   = L.RESF (+ later); waterfall = H.FLLS/OVF; glacier = H.GLCR; island =
   T.ISL/ISLS; canyon = T.CNYN/GRGE/VAL; spring = H.SPNG/HTSP/GYSR.
2. *Stored data* — the main `places` table gains `fcode` (the exact
   GeoNames code) so the category → code join is local; every landmark row
   already carries `distance_m`. New `scanned_files.shore_distance_m`: the
   distance to the nearest Natural Earth 10 m shoreline segment
   (coastline, lake shores, major rivers; public domain), computed by the
   offline resolver from a `shore` table the gazetteer build now embeds
   (line segments bucketed by 0.1° cell). NULL when > 5 km or the table is
   absent. Only `waterfront` is flagged `shoreline`: the stored distance is
   to ANY water edge (coast, lake, river), so "lake" and "beach" stay on
   GeoNames feature points rather than matching a sea cliff. This is what makes "waterfront" true on the shore of a bay
   whose GeoNames point is 2 km out in the water.
3. *Distance semantics* — two ops on the new `setting` dimension: `at`
   (within `search.places.settingAtM`, default 150 m) and `near` (within
   the landmark radius, 1.5 km). The LLM picks the op from the phrasing:
   "on the waterfront" / bare "waterfronts" → at; "near the water",
   "close to the beach" → near.
4. *Query* — `setting` is a registry dimension (prompt schema + grammar
   generated; hints = the category names). The validator grounds the value
   (singular/plural folded: "waterfronts" → waterfront), requires a known
   category, and emits `ExifQueryConstraints::settings` = {codes, max
   metres, uses_shoreline}. A setting fact is BOUND; like an inferred place
   it is `inferred` when the span carries no scaffolding ("beach photos"),
   so an empty intersection retries once without the inferred facts (a
   library with no GPS still gets its CLIP answer). The regex fallback
   parser does not know settings (same as file types).
5. *SQL* — `id IN (SELECT fp.file_id FROM file_places fp JOIN places p ON
   p.id = fp.place_id WHERE p.fcode IN (...) AND fp.distance_m <= X)`, OR
   `shore_distance_m <= X` for categories flagged `shoreline` (waterfront). Settings AND with the place: "new jersey waterfronts" =
   NJ photos ∩ within 150 m of water. The setting word stays in the CLIP
   text so ranking inside the geo set is visual.
6. *Backfill* — the place sweep already re-runs on level-0 rows; bumping
   the resolver version (`PlacesOps::kLevelOffline`, 1 → 2) re-sweeps
   existing rows so `fcode` and `shore_distance_m` fill in on the next
   integrity check.
7. *Deterministic detection* (found in the golden run) — the shipped 1.5B
   model files "waterfront"/"lake"/"mountains" as location VALUES or
   leaves them in the semantic text; it emitted no setting fact at all for
   "new jersey waterfronts". Setting categories are a closed vocabulary, so
   `detectSettings()` (search/setting_detector) finds them in code on BOTH
   parser paths: a category token becomes a setting; a scaffold word in the
   two preceding tokens picks the op (near/close/by/around/along/beside/in
   → near, on/at → at) and makes it explicit; a bare token is `at` +
   inferred. The validator converts model location facts whose value is a
   category into settings, and skips tokens inside accepted fact spans
   ("salt lake city" never yields "lake"). The model's own setting fact,
   when it does emit one, keeps its op.

**Live numbers (author's library copy):** 16 files within 150 m of a
Natural Earth shoreline, 789 within 1.5 km. The NE 10 m coastline is coarse
around the Hudson (the 549 Harsimus Cove photos qualify through the
GeoNames cove point at 80 m, not the shoreline) — exactly why both signals
are OR-ed.

### 9.3 Code-review fixes (2026-09-20)

Six findings from the cross-file review, all fixed with regression tests:

1. Regex parser stopped its location pass only on a bbox hit, so a
   place_ids-only match ("in bozeman") was overwritten by a later hit
   ("near yellowstone"). Now any resolved hit ends the pass.
2. "in mountain view" (an unresolvable place) was re-read as a
   non-inferred "near mountain" setting. Unresolved place attempts are now
   passed to the detector as covered spans on both paths; a span that is
   itself a setting ("in the mountains") stays visible.
3. Only the 5 nearest landmark rows were stored, so a lake 600 m away
   behind five closer features never matched "near the lake". The cap now
   also keeps the nearest feature of every kind.
4. Shore segments were bucketed under their bbox CORNER cells only; long
   segments were missing from their middle cells while the reader scans
   ±1 cell. Every cell the bbox covers is now written (rebuilt: 1.14 M
   rows).
5. `location_inferred` was OR-ed, so an explicit "in bozeman" after an
   inferred "yellowstone" inherited the retry and could be dropped. It is
   now assigned by the last location fact.
6. The boot sweep and the integrity-check sweep share one lock; the second
   caller used to return silently with empty stats (the dialog showed the
   phase with nothing happening). It now waits for the running sweep, then
   reports the true remaining count. The boot sweep runs on an owned
   thread that shutdown stops and joins.

## 10. Phases

| phase | scope | deliverable |
|---|---|---|
| **PLACE-1** | places schema + `build_places_db.py` + `OfflineReverseGeocoder` + scan-time hook + backfill sweep + `place_label` in `DayFile`/viewer | every GPS photo has an offline name; labels visible |
| **PLACE-2** | search resolves through places-with-photos ∪ bbox; `unresolved_places` in response; golden tests | "in bozeman", "in banff", "lower falls" work offline |
| **PLACE-3** | consent config + Settings › Places + inline prompt + gRPC work/submit RPCs + Swift `PlaceResolver` with Apple provider + clear/revoke | neighbourhood/POI names, park polygons via `areasOfInterest`, all opt-in |
| **PLACE-4** | setting categories (waterfront/beach/lake/mountain/...), `places.fcode`, Natural Earth shoreline distance, `setting` LLM dimension with at/near ops | "new jersey waterfronts" = NJ ∩ at the water; "near the beach" |
| later | Nominatim provider; PLACES tab; video GPS | — |

## 11. Decisions (review 2026-09-19)

Resolved:
- **Gazetteer size**: measure what fits in 1 GB → §3.1; curated 732 MB build
  recommended, ~270 MB headroom for admin polygons + alternate names.
- **Bare place names**: LLM-inferred, bound, with the §6.1 gates. Merge
  algebra unchanged.
- Deferred by the user until later: provider order (Apple vs Nominatim),
  coordinate precision sent, consent surface, consent-flag ownership.

Originally asked:

1. **Offline gazetteer size**: cities500 + parks + waterfalls/peaks/lakes,
   target ≤ 40 MB committed. OK to grow the repo/app by that, or prefer a
   smaller "cities15000 + features" (~8 MB) and lean on the online layer?
2. **Online provider order**: Apple first, Nominatim later. Or Nominatim
   first because it's fully open / no Apple dependency?
3. **Coordinate precision sent online**: cluster centroid at 0.001° (~110 m).
   Coarser (0.01°, ~1 km) is more private but loses POI/landmark names.
4. **Consent default**: `ask` with the inline prompt on the first unresolved
   place name (recommended) vs. a one-time Settings-only toggle with no
   inline prompt.
5. **Server config vs. client preference** for the consent flag: proposed
   server config (single source of truth, shows in Configuration, survives
   client reinstall). Downside: a paired second client inherits the choice.
6. **Bare place tokens** ("yellowstone waterfalls" without a preposition):
   keep the EXIF-7 rule (semantic only) for v1, or enable the bare-union
   branch for names that have photos?
