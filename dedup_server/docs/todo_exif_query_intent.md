# EXIF-7 — Query Intent: Bound Constraints vs Bare Facts

Status: designed 2026-08-23, not implemented. Follows the union-search change
on `feature/search_type_badges` (per-file ai/xf badges, DayFile
`matched_ai`/`matched_exif`).

**2026-08-23 update:** the §4 regex-grammar expansion is SUPERSEDED by
QP-1 (local LLM query structurer) — §4 now
defines the target *semantics* the structurer must produce, not a regex
implementation plan. The §3 result algebra, badges, and all user decisions
here remain authoritative and are implemented in QP-1a against a parser
interface; the existing regex parser stays as fallback and test baseline.

## 1. Problem

The union change fixed "f2.5" (a bare fact now also surfaces CLIP results),
but it broke the intuition of well-formed queries: "sunset taken with fuji
between 2018 and 2024" clearly states constraints, yet today it returns the
superset — every fuji photo and every CLIP sunset hit. The user's model:

- **"raw, sunset, fuji"** — a loose list of terms → **union** (discovery).
- **"sunset taken with fuji between 2024 and 2018"** — grammatically bound
  constraints → **intersection** (filter).

The parser already distinguishes these mechanically: a fact matched *with
grammatical scaffolding* (context verbs/prepositions/articles, comparison
words, range wording) versus a *bare token*. EXIF-7 turns that distinction
into result semantics.

## 2. Intent model

Every parsed fact gets a **binding**:

- **Bound** — matched with scaffolding. Expresses filter intent → hard SQL
  constraint, intersected with everything else.
- **Bare** — the token alone. Exploratory → soft term, unioned into the
  result alongside the semantic (CLIP) hits.

### Binding classification per fact type

| Fact type    | Bound (intersect)                                               | Bare (union)                    |
|--------------|-----------------------------------------------------------------|---------------------------------|
| Camera brand | scaffolding participated: "taken with fuji", "with a fuji", "on my iphone", "fuji camera" | bare token: "fuji"              |
| ISO / aperture / shutter / focal | comparative or range forms: "iso above 800", "wider than f/2.8", "iso between 400 and 1600", "faster than 1/500" | exact-value probes: "iso 800", "f/2.5", "50mm", "shutter 1/500" |
| Flash        | always ("with flash" / "without flash" are inherently scaffolded) | — (no bare form)                |
| Location     | always (preposition-triggered only: "in yellowstone")            | — (bare place names stay semantic; "Nice"/"Phoenix" rule unchanged) |
| Dates        | **always** (user decision 2026-08-23): "in august 2024", "between 2018 and 2024", AND bare "2024" / bare "august" — a date token has no visual meaning for CLIP, so a hard filter never surprises. Bare "may" keeps its ambiguity exemption (stays semantic). | — |
| File type (new, §5) | with an exclusivity word: "only raw files"                | bare: "raw", "jpeg", "video"    |

**Decision to confirm:** exact-value numeric probes ("iso 800") are classified
*bare* even though they name the field, because they are approximate matches
(f/2.5 matches 2.4–2.6) and typically exploratory — consistent with the
motivating "f2.5" case. Veto here if "iso 800" should filter instead.

Mechanically: binding falls out of existing structure — the camera regex's
scaffolding groups become capturing groups checked via `sub_match::matched`;
the numeric comparative vs exact patterns are already separate compiled
regexes; dates/location/flash are constant per type.

## 3. Result algebra

Let, after parsing the whole query:
- **B** = bound constraints → one SQL WHERE: **AND across fields, OR within a
  field's values** (user decision: "canon, nikon" = canon OR nikon; today's
  AND-of-LIKEs can never match and is a live bug).
- **S** = bare facts → per-field SQL sets (OR within field), each a union branch.
- **R** = semantic text pool (non-fact segments / leftover, filler-stripped).

| Case | Query shape | Result |
|------|-------------|--------|
| 1 | B=∅, S=∅, R≠∅ ("sunset over water") | CLIP(R) — unchanged pure semantic |
| 2 | B=∅, S≠∅ ("f2.5"; "raw, sunset, fuji") | CLIP(R, or raw query if R is filler) ∪ ⋃ SQL(Sᶠ) |
| 3 | B≠∅ and (R≠∅ or S≠∅) ("sunset with a canon"; "sunset taken with fuji, f2.5") | ( CLIP(R) ∪ ⋃ SQL(Sᶠ) ) ∩ SQL(B) |
| 4 | B≠∅, R filler, S=∅ ("taken with fuji in 2024") | SQL(B) only — **AI half skipped** (user decision), xf-only badges; restores the fast pre-union SQL path |

Degradation: if the CLIP half is down (no model/tokenizer/index), cases 2–3
answer from the EXIF sets alone with `used_semantic=false` — never an error
when SQL can answer (already implemented in the union change; keep it).
In case 3 a starved CLIP result (nothing above `search.minScore`) yields an
honestly small/empty intersection — filter intent means we do NOT fall back
to the superset; log the counts.

### Worked examples

- "raw, sunset, fuji" → S={type:raw, camera:fuji}, R="sunset" →
  CLIP(sunset) ∪ raw-files ∪ fuji-photos. Badges vary per tile.
- "sunset taken with fuji between 2024 and 2018" → B={camera:fuji,
  year∈[2018,2024]} (reversed bounds normalized), R="sunset" →
  CLIP(sunset) ∩ B.
- "sunset with a canon" → B={camera:canon} → CLIP(sunset) ∩ canon. (Fixes
  the regression the union change introduced.)
- "f2.5" → S={aperture≈2.5}, R filler → CLIP("f2.5") ∪ f/2.5-photos.
- "canon, nikon" → S={camera: canon∨nikon}, R filler → CLIP(raw query) ∪
  (canon ∨ nikon).
- "sunset 2024" → B={year 2024}, R="sunset" → CLIP(sunset) ∩ 2024.
- "f2.5 to f12 but taken with fuji or canon between 2005 and 2015" →
  B={f∈[2.5,12], camera∈{fuji,canon}, year∈[2005,2015]}, R=filler, S=∅ →
  case 4: pure SQL intersection, AI skipped, xf-only badges.
- "Flowers, taken at f2.5 to f12, with fuji or canon, between 2005 and
  2015" → same B, but R="flowers" → case 3: CLIP(flowers) ∩ SQL(B); every
  result badges ai+xf.

## 4. New grammar: ranges, value lists, connectives

Motivating query (2026-08-23): "f2.5 to f12 but taken with fuji or canon
between 2005 and 2015". Today this collapses to contradictory ANDs (f≈2.5 ∧
f≈12, fuji ∧ canon) plus CLIP on the leaked remnant "to but or". Three
grammar additions fix it:

**4.1 Ranges (bound), for dates AND numeric fields:**
- "between X and Y", "from X to Y", "X to Y" — where both X and Y are the
  same value kind: years ("between 2005 and 2015"), f-stops ("f2.5 to
  f12"), ISO (subsumes the existing "iso between"), shutter ("1/500 to
  1/60"), focal ("24mm to 70mm").
- **Normalize reversed order** ("between 2024 and 2018" → [2018, 2024]; for
  shutter, remember smaller exposure time = faster).
- A range is always **bound** — range wording is scaffolding by definition.
- Bare hyphen "2018-2024" deliberately NOT matched (collides with filenames
  and ID-like tokens); revisit on demand.

**4.2 Within-field value lists via connectives:**
- "taken with fuji or canon", "canon or nikon", "raw or jpeg" — "or" (and
  pragmatically "and": a photo has one camera, so "canon and nikon" between
  same-field values also reads as OR) join values of ONE field into a value
  list, same semantics as the §3 within-field OR for commas.
- Scaffolding distributes over the whole list: "taken with fuji or canon" is
  a single BOUND camera constraint with values {fuji, canon}.
- Cross-FIELD boolean ("f2.5 or taken with fuji") is NOT in scope — flat
  AND/union semantics only; parentheses/negation stay in a future EXIF-8.

**4.3 Connective vocabulary:**
- "but", "or", "to", "between", "from", "either" join the keyword JSON as a
  new `connectives` list: consumed by the shapes above, and stripped as
  filler when they survive parsing so they never leak into the CLIP text
  (today "but"/"or"/"to" are not filler and would be embedded).
- Numeric facts gain optional scaffolding prepositions ("taken at f2.5",
  "shot at iso 800") — "at"/"with" before a numeric fact are part of the
  match, not location triggers. Fact patterns run BEFORE the location
  pattern, so "at f2.5" can never reach the geocoder.
- **Orphaned-scaffold stripping**: when a fact consumes a segment, leftover
  scaffolding words in that segment ("taken", "at") are stripped as filler
  even when a fact pattern didn't swallow them — a constraint segment must
  contribute zero tokens to the CLIP text.
- All wording is JSON-extensible per EXIF-6 rules.

## 5. New fact dimension: file type (user decision: in scope)

- Keyword JSON gains `file_types`: `{ "raw": ["cr2","cr3","nef","arw","raf",
  "orf","rw2","dng"], "jpeg": ["jpg","jpeg"], "heic": ["heic","heif"],
  "png": ["png"], "tiff": ["tif","tiff"], "video": ["mp4","mov","avi","mkv",
  "m4v"] }` — names and lists fully user-extensible.
- Matches against the `scanned_files.file_name` extension (SQL:
  `lower(file_name) LIKE '%.' || ext`); consider a generated/indexed
  extension column if it profiles poorly at library scale.
- Bare "raw" = soft; "only raw files" (exclusivity vocabulary, also in JSON)
  = bound.

## 6. Segmentation and the semantic pool

- Split the query on commas/semicolons into segments BEFORE parsing; parse
  each segment; merge fact sets. A segment's leftover non-filler text joins
  the semantic pool R.
- Multi-embedding: each non-filler semantic segment is embedded separately
  (cap: 4 segments) and their KNN hits unioned, keeping each file's max
  score. "raw, sunset, fuji" then finds Mt-Fuji-looking photos via
  CLIP("fuji") as well as fuji-branded EXIF rows — the badges disambiguate.
  Cost is one ~ms ONNX inference per segment.
- Without commas, facts are still extracted positionally (unchanged) and R
  stays a single string — commas refine semantics, they are not required.
- Cache key stays the raw trimmed query (unchanged).

## 7. API / UI

- **No proto change required.** `DayFile.matched_ai`/`matched_exif` are
  already per-file; response `search_used_ai`/`search_used_exif` keep meaning
  "which halves ran". In case 4 every badge is xf-only, which visually
  communicates "pure filter" for free.
- Optional later: a `search_mode` (FILTER vs DISCOVER) response enum for a
  search-bar hint; not part of EXIF-7.

## 8. Libraries considered (decision: no new dependency now)

- **cpp-peglib** (yhirose, MIT, single header): a declarative PEG grammar
  would be the right tool IF the grammar grows boolean operators, negation,
  or nesting ("(canon or nikon) and not 2020"). EXIF-7's grammar stays a
  flat set of anchored patterns plus one classification bit per match —
  capture-group participation on the existing compiled regexes covers it,
  and it preserves the EXIF-6 JSON-vocabulary design (word lists splice into
  fixed shapes). Adopt peglib only if an EXIF-8 boolean-syntax feature lands.
- **SQLite FTS5**: full-text indexing of camera/path strings is orthogonal to
  intent; current bound LIKEs on indexed rows are fine at this scale.
- **Heavy NLP** (spaCy/CoreNLP/UDPipe): dependency parsing for a closed,
  12-shape grammar is the wrong trade for an offline C++ server — large
  runtime, nondeterministic upgrades, no JSON-extensible vocabulary.
- **LLM query rewriting**: rejected — latency, nondeterminism, offline
  constraint.

## 9. Edge cases

- Reversed ranges ("between 2024 and 2018") → normalize, never empty-by-accident.
- Conflicting bound constraints ("iso above 800 iso below 200") → allowed,
  honest empty result, info log.
- Same field bound twice with values ("in 2023, in 2024") → OR within the
  date dimension (union of ranges), consistent with §3's within-field OR.
- Bare "may" → stays semantic (ambiguity exemption survives the bare-dates-
  are-bound decision); "in may" → bound month.
- Bare brand homonyms ("fuji" the mountain, "pixel" the art style) → soft by
  design; CLIP keeps the non-brand reading alive in the union.
- Bare place names still need prepositions (unchanged FILT rule for
  "Nice"/"Phoenix").
- Union size: keep `search.exifOnlyLimit` as a per-branch cap and add a total
  result cap (reuse `exifOnlyLimit`) applied after merging, CLIP-ranked first.
- Duplicate facts across segments ("fuji, taken with fuji") → the bound
  instance wins (a fact that appears both bound and bare is bound).
- NULL EXIF never matches; flash unknown ≠ off (both unchanged invariants).
- Clause scoping: "but"/", " clauses do not create cross-field alternation —
  all bound constraints AND globally. For "A but B", flat AND matches intent;
  true cross-field OR is EXIF-8 territory.
- Range endpoints of mixed kinds ("f2.5 to 2015") → not a range; each side
  parses independently by its own kind.
- Negation beyond flash ("not canon", "except 2020") → out of scope; note
  for a possible boolean EXIF-8.

## 10. Implementation phases

1. **EXIF-7a parser**: per-fact binding on `ExifQueryConstraints` (numeric /
   camera / dates / file-type entries become structs with `bound`), comma
   segmentation, range grammar for dates and numeric fields (§4.1),
   connective value lists (§4.2), connective vocabulary (§4.3), file-type
   facts, within-field value lists (OR), bound-wins-over-bare dedup.
2. **EXIF-7b engine**: implement the §3 algebra in `searchUncached`
   (bound WHERE builder with within-field OR, soft branches, multi-embedding
   pool, case-4 SQL-only path, degradation rules).
3. **EXIF-7c vocabulary**: JSON sections `file_types`, exclusivity words,
   range wording; defaults + shipped file + lockstep test extended.
4. **EXIF-7d tests**: parser binding-classification matrix (every row of the
   §2 table), e2e per §3 case (hermetic: model path pinned nonexistent),
   worked examples as tests, within-field OR, reversed range, bound-wins
   dedup, golden compatibility for pure semantic and case-4 queries.
5. **EXIF-7e**: docs, memory, rollout note (server restart only — no schema
   or proto change).

## 11. Acceptance

- "sunset with a canon" → only canon sunsets (intersection restored).
- "f2.5" → superset with mixed badges (union preserved).
- "raw, sunset, fuji" → union of raw files, fuji EXIF rows, CLIP sunset/fuji.
- "sunset taken with fuji between 2024 and 2018" → fuji ∧ [2018,2024] ∧
  CLIP(sunset).
- "canon, nikon" returns results (within-field OR bug fixed).
- "f2.5 to f12 but taken with fuji or canon between 2005 and 2015" →
  f∈[2.5,12] ∧ (fuji ∨ canon) ∧ year∈[2005,2015], no CLIP noise from
  "to but or".
- All existing suites green; e2e stays hermetic.
