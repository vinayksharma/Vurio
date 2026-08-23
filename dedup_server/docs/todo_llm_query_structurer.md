# QP-1 — Local LLM Query Structurer

Status: designed 2026-08-23; **QP-1a implemented 2026-08-23** on
`feature/qp1_query_intent` (StructuredQuery + intent-split regex parser +
engine algebra; 391 tests pass). QP-1b–e (llama.cpp structurer) not started. Supersedes the §4 regex-grammar
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
  (default 2000). Missing file → warn once, regex fallback; the feature is
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
