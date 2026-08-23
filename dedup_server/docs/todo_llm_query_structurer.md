# QP-1 — Local LLM Query Structurer

Status: designed 2026-08-23. **QP-1a implemented** (StructuredQuery +
intent-split regex parser + engine algebra). **QP-1b implemented
2026-08-23**: `LlmRuntime` (include/search/llm_runtime.hpp) wraps llama.cpp
pinned at b4589 via CMake FetchContent (Metal, embedded shaders) — lazy
load, mutex-serialized context, greedy decode, GBNF-constrained sampling
with a root-rule pre-check (llama.cpp silently no-ops an unparseable
grammar — refused instead), deadline abort, KV prefix reuse, chat template
from GGUF metadata with ChatML fallback, `search.llm.*` config keys.
Hermetic tests plus model-gated smokes (VURIO_TEST_GGUF; verified against
llama.cpp's CI stories260K model: constrained decode, determinism, timeout,
recovery). **QP-1c implemented 2026-08-23** on `feature/qp1c_structurer_schema`:
`QueryFactSchema` (include/search/query_fact_schema.hpp) is the dimension
registry — 9 dimensions (camera, iso, aperture, shutter, focal, flash,
date, location, file_type; person reserved for QP-2), hints drawn from the
keyword JSON (which gained a `file_types` map, kept sorted). The registry
GENERATES the system-prompt schema section and the GBNF grammar; every
grammar repetition is bounded ({m,n}) so constrained decodes always
terminate. `LlmQueryStructurer` owns the byte-stable system prompt
(rules + schema + 9 few-shot examples from the worked-example set) and
parses the grammar-guaranteed JSON into RawOutput via an injected
completion function (hermetic tests) or LlmRuntime. A model-gated smoke
proves the generated grammar forces even a storybook model to emit
contract-conforming JSON. **QP-1d implemented 2026-08-23** on
`feature/qp1d_validator_wiring`: `LlmFactValidator`
(include/search/llm_fact_validator.hpp) is the trust boundary — span
grounding (fact span must occur in the query, every value inside its
span; clip_queries grounded too, capped at 4), registry checks (dims,
per-dim ops, sanity ranges, reversed-range normalization), resolution
(file_type via the keyword JSON's file_types → new
`ExifQueryConstraints::file_extensions` + case-insensitive extension SQL;
locations via gazetteer, unresolved places demoted to clip_queries).
Engine wiring: LLM structurer is the PRIMARY parser with the full ladder
(disabled/model-missing/error/timeout/fully-rejected → regex parser);
`clipSearchUnion` implements multi-segment CLIP (§6, max score per file);
`buildExifWhere` camera terms now OR within the field (EXIF-7 §3).
Test seam `setLlmCompleteFnForTests` exercises the ladder hermetically.
The e2e fixture pins `search.llm.enabled=false` (a real GGUF now lives in
models/). Golden tests (VURIO_GOLDEN_GGUF → the shipped Qwen2.5-1.5B
q4_k_m, downloaded 2026-08-23): the three worked examples structure
exactly as specified, and the full engine pipeline answers "raw files
from 2019" — a query the regex parser cannot express. **QP-1e completed
2026-08-23**: 8-query acceptance matrix (GoldenMatrixTest) over the real
model asserting the VALIDATED StructuredQuery — every EXIF-7 §11
acceptance item passes, including the reversed range and both difficult
queries. Model tuning that came out of it: two few-shot examples
(month+year as one value + flash; focal-with-unit + location-vs-date
disambiguation) and three deterministic validator normalizations (unit
stripping "85mm"/"f/2.8"/"1/500s"; split month/year values recombined;
bare-year value refined to a month range when the grounded span names a
month). ~1.5 s/query after a one-time ~2 s model load; hermetic suite
never touches the model. **QP-1 is complete** — remaining is live
verification (restart server, search VU) and QP-2 (person dimension,
once the face DB exists). Supersedes the §4 regex-grammar
expansion of [todo_exif_query_intent.md](todo_exif_query_intent.md) (EXIF-7);
EXIF-7's result algebra (§3), badges, and user decisions remain the target
semantics and move unchanged into this plan's engine phase.

## 1. Why

The EXIF-7 regex grammar was nearing its complexity ceiling (ranges,
connectives, orphaned-scaffold stripping, preposition collisions — each new
query shape added rules). Two coming needs break it outright:

- **Open vocabulary**: face recognition means "photos of mom and dad at the
  beach in 2019" — person names have no word list.
- **Intent nuance**: bound-vs-bare classification ("sunset with a canon" vs
  "raw, sunset, fuji") is a language judgment, not a token pattern.

Decision (user, 2026-08-23): a small local quantized instruct model
structures the query; the deterministic parser stays as fallback and test
baseline. **The LLM proposes, deterministic code verifies.**

## 2. Architecture

```
query ──► MRU cache ──► LlmQueryStructurer ──► Validator ──► StructuredQuery
                 │   (llama.cpp, GBNF JSON)   (grounding,        │
                 │            │ timeout/fail   registry checks)  ▼
                 │            ▼                          engine algebra
                 └──► ExifQueryParser (regex fallback) ──► (EXIF-7 §3, unchanged)
```

- **Parser interface**: `StructuredQuery { facts[] (dimension, op, values,
  intent=filter|explore, span), clip_queries[] }`. Two producers:
  `LlmQueryStructurer` (primary) and the existing `ExifQueryParser` adapted
  to emit the same type (fallback + golden baseline). The engine (union /
  intersection algebra, badges, degradation) never knows who parsed.
- **Dimension registry**: each fact dimension registers name, value type,
  SQL resolver, validator, and availability — camera, iso, aperture,
  shutter, focal, flash, date, location, file_type today; `person` later
  (registered only when the face DB exists). The registry generates both
  the LLM prompt's schema section and the GBNF grammar, so adding a
  dimension is a registry entry, never a grammar rewrite.

## 3. Runtime: llama.cpp

- **Library**: llama.cpp (MIT, pure C/C++), pinned release via CMake
  FetchContent or git submodule; Metal backend on macOS. Chosen over ONNX
  Runtime GenAI for maturity of C++ embedding, GGUF quantization, and
  **GBNF grammar-constrained decoding** — output conforming to our JSON
  schema is enforced at the sampler, so malformed JSON is impossible.
- **Model** (user decision): ~1.5B instruct, Q4 GGUF — default
  Qwen2.5-1.5B-Instruct Q4_K_M (Apache-2.0, ~1.0 GB). Swappable via config.
- **Config**: `search.llm.modelPath` (default
  `models/qwen2.5-1.5b-instruct-q4_k_m.gguf`, project-root resolved like the
  CLIP model), `search.llm.enabled` (default true), `search.llm.timeoutMs`
  (default 0 = no total cap; total cost is bounded by prompt size + grammar-bounded output and scales with the hardware, so a fixed cap tuned on one machine is a regex-fallback trap on a slower one. `search.llm.stallTimeoutMs`, default 10000, guards progress instead: abort only when a 128-token prefill batch or one generated token makes no progress in the window — that trips on wedges, never on slow hardware). Missing file → warn once, regex fallback; the feature is
  additive exactly like the CLIP model file.
- **Decoding**: greedy, temperature 0 → deterministic for a given query.
  System prompt (schema + few-shot) is prefix/KV-cached once; per query only
  the user text is prefilled and ~100–150 JSON tokens decoded. Expected
  0.4–1s on Apple Silicon; the 300 ms search debounce and the existing MRU
  query cache sit in front, so typing is unaffected and repeats are free.
- **Threading**: one llama context, mutex-guarded (contexts are not
  thread-safe); inference on the gRPC handler thread like CLIP embedding;
  timeout via llama.cpp's abort callback → fallback.
- **Memory**: ~1–1.5 GB when loaded; lazy-load on first search. Optional
  later: unload after idle.

## 4. Prompt and output contract

- **System prompt** (generated from registry + keyword JSON): task
  description, dimension schema with allowed ops, known camera brands and
  file types as *hints* (not closed sets — camera matches by LIKE), and
  few-shot examples drawn from EXIF-7's worked examples, including both
  "difficult" queries:
  - "f2.5 to f12 but taken with fuji or canon between 2005 and 2015" → all
    facts intent=filter, clip_queries=[].
  - "Flowers, taken at f2.5 to f12, with fuji or canon, between 2005 and
    2015" → same facts, clip_queries=["flowers"].
  - "raw, sunset, fuji" → facts file_type:raw + camera:fuji intent=explore,
    clip_queries=["sunset", "fuji"].
  - "f2.5" → aperture≈2.5 intent=explore, clip_queries=["f2.5"].
- **Output** (GBNF-constrained JSON), e.g.:

```json
{ "facts": [
    {"dim":"aperture","op":"range","min":2.5,"max":12,"intent":"filter","span":"f2.5 to f12"},
    {"dim":"camera","op":"in","values":["fuji","canon"],"intent":"filter","span":"with fuji or canon"},
    {"dim":"date","op":"range","min":"2005","max":"2015","intent":"filter","span":"between 2005 and 2015"} ],
  "clip_queries": ["flowers"] }
```

- `intent` maps 1:1 onto EXIF-7's bound (filter) / bare (explore) algebra.
- `clip_queries` replaces comma segmentation: the model emits the semantic
  segments directly (multi-embedding union per EXIF-7 §6, cap 4).

## 5. Validation (deterministic, the trust boundary)

Every LLM output passes plain-code checks before touching SQL:

1. **Shape**: guaranteed by GBNF; still schema-validated defensively.
2. **Span grounding** (anti-hallucination core): each fact's `span` must
   occur in the query (case/whitespace-normalized), and its values must be
   derivable from that span (numbers/dates appear literally; camera values
   appear as tokens). A fact whose value isn't in the text does not exist.
3. **Registry validation**: numbers parse and are sane (ISO 25–10⁶,
   aperture 0.7–64, years 1900–2100), reversed ranges normalized, unknown
   `dim` dropped, location names must resolve in the gazetteer, person
   names (later) must resolve in the face DB — an unresolved person name
   moves to `clip_queries` instead of silently filtering to zero.
4. **Fallback ladder**: invalid fact → dropped (logged). All facts invalid,
   parse failure, or timeout → regex parser result. Model file absent →
   regex parser, warn once. The regex path also remains the hermetic-test
   parser, so CI never needs the model.

## 6. Faces (QP-2 forward-compatibility, design constraint only)

`person` becomes a registry dimension: resolver maps names → person IDs →
SQL join on the face-detection table; intent rules identical ("of mom and
dad" = filter; bare "mom" = explore, unioned with CLIP). Nothing in QP-1
implements it, but the registry, schema generation, and validator are built
so QP-2 is an entry, not a refactor.

## 7. Testing

- **Validator unit tests** (no model): canned LLM outputs — correct,
  hallucinated value (span not in query), ungrounded span, unknown dim,
  reversed range, absurd numbers, unresolved location — assert drops and
  fallbacks.
- **Structurer unit tests** (no model): `LlmQueryStructurer` takes a
  completion functor; a fake returns canned JSON to test prompt assembly,
  timeout, and the fallback ladder.
- **Golden parity** (model-gated, skipped when absent): EXIF-7's worked
  examples + acceptance list through the real model must produce the
  expected StructuredQuery.
- **Engine e2e**: unchanged and hermetic — model path pinned nonexistent,
  regex fallback exercises the full algebra (EXIF-7 §10 test plan).

## 8. Implementation phases

1. **QP-1a**: parser interface + `StructuredQuery`; adapt `ExifQueryParser`
   to emit it; implement the EXIF-7 §3 engine algebra against the interface
   (delivers EXIF-7 behavior on the regex path, minus its retired §4).
2. **QP-1b**: llama.cpp build integration, config keys, lazy load, mutex,
   timeout, prefix cache.
3. **QP-1c**: dimension registry; prompt + GBNF generation from it;
   few-shot set.
4. **QP-1d**: validation layer + fallback ladder.
5. **QP-1e**: tests, docs, memory. Acceptance = EXIF-7 §11 list, both
   difficult queries structured correctly via the LLM path, and full suite
   green with the model absent.

## 9. Rollout

Place the GGUF in `models/` beside the CLIP ONNX files and restart — no
schema or proto change. Without the file the system behaves exactly as the
regex parser allows, so shipping the code and shipping the model are
independent steps.
