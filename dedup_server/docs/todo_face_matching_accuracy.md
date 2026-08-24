# FACE-7 — Matching Accuracy: same-name merge, multi-exemplar clusters, join margin, outlier review

Status: designed 2026-08-24, not yet implemented.
Depends on: FACE-2 (clusterer), FACE-3 (naming/merge), FACE-3b (cannot-links),
FACE-6 (pinned covers).

## 1. Motivation

Three accuracy gaps, all rooted in the same representation choice — a cluster
is summarized by **one** size-weighted mean centroid:

1. **Same-person clusters persist.** Alice-in-2010 and Alice-in-2024 form two
   clusters whose centroids never reach the 0.65 auto-merge bar. Naming both
   "Alice" already unifies them for search (both map to one `persons` row and
   the person filter runs on `faces.person_id`), but the PEOPLE grid shows two
   cards forever, and each cluster matches new faces with only half the
   evidence.
2. **The blended mean is a weak matcher.** Merging two visual modes of one
   person produces a centroid between them that may represent neither well —
   it can sit closer to a stranger than any real photo of the person does.
   This is why FACE-3's forced merge can *hurt* subsequent matching, and why
   false positives join: a face only has to beat one diluted average, with no
   check that it isn't almost as close to a different person.
3. **User knowledge is unused by matching.** The FACE-6 pinned cover is a
   human-verified "this is definitely them" example, but today it is purely
   presentational — matching never sees it.

FACE-7 keeps the centroid (it is a good exemplar for the dominant mode and
the cheap pre-filter) and adds a small set of **exemplars** per cluster, then
builds four features on top of them.

## 2. Design

### 2.1 FACE-7a — same-name commit merges the person's clusters

Naming a cluster is a deliberate identity statement, stronger than any
threshold. When `NameFaceCluster` (or the popup's inline rename) assigns a
name that other clusters already carry (same `person_id` after
`nameCluster`), the server merges them: **all of the person's clusters merge
into the largest** (ties: lowest id), via the existing
`FacesOps::mergeClusters` — FACE-6's pin rules already make a pinned cover
survive regardless of merge direction.

- The merge is unconditional — no centroid sanity bar. The user typed the
  same name twice; a typo'd merge is on them (there is no unmerge; the spec
  accepts this because the act is explicit and repeated).
- **Bug fix folded in:** the DB-side `mergeClusters` never repointed
  cannot-links (only the clusterer's in-memory consolidation did, via
  `repointCannotLinks`). A "not this person" recorded against a source
  cluster silently died on any user-forced merge. `mergeClusters` now calls
  `repointCannotLinks(source, target)` before deleting each source, so the
  constraint follows the identity — same rule as consolidation.
- The response reloads the PEOPLE list as usual; the two cards collapse into
  one on the next paint. Aliases are unaffected (they live on the person).

### 2.2 FACE-7b — multi-exemplar cluster representation

Each cluster gains up to **K = 5** exemplar faces alongside the centroid.

**Selection** (deterministic, recomputed rather than incrementally patched):
sort members by quality desc; walk the list, adopting a face as an exemplar
when its cosine to every already-adopted exemplar is `< 0.80`
(`exemplar_spread`) and its quality is `>= 0.4` (`exemplar_min_quality`);
stop at K. This is farthest-point sampling with quality priority: the first
exemplar is the best face, later ones only enter when they represent a
genuinely different look (age, glasses, lighting). The FACE-6 **pinned cover
face is always exemplar #0** when it has an embedding — the human-verified
anchor joins the match set unconditionally (this is how "picking a better
cover" now improves matching, answering the FACE-6 follow-up question).

**When selection runs:** inside `recomputeCluster` (already triggered by
face removal), after any merge (DB and in-memory), and for clusters touched
by `assign` — but recomputation from members needs the members' embeddings,
so the clusterer refreshes a cluster's exemplars lazily: a cluster whose
size changed since its exemplars were computed re-selects on the next
`consolidate()` pass (every 40 ticks / on caught-up), reading that cluster's
member embeddings only. Cost: touched clusters × members × K cosines.

**Storage:** `face_cluster_exemplars (cluster_id INTEGER, face_id INTEGER,
PRIMARY KEY (cluster_id, face_id)) WITHOUT ROWID` — ids only; embeddings
stay in `faces`. Loaded with the cluster mirror at tick start (a join
against `faces.embedding`). Rows for a cluster are replaced wholesale on
re-selection; deleted with the cluster.

**Scoring:** a face's similarity to a cluster becomes
`score = max(cos(e, centroid), max_i cos(e, exemplar_i))`.
`assign()` joins on this score; everything downstream (margin, fit) uses the
same definition. Memory: 7k clusters × 6 vectors × 512 floats ≈ 86 MB worst
case — acceptable; assign stays a brute-force scan.

**Consolidation with exemplars** — exemplar-max matching must not make
merging trigger-happy (one noisy exemplar pair could fuse two strangers), so
merging keeps a two-stage test:
- pre-filter: centroid cosine `>= 0.35` (`merge_prefilter`) — cheap, prunes
  the O(n²) pass; deliberately below the join threshold because two looks
  of one person can have distant blended centroids;
- verify: merge when centroid cosine `>= merge_threshold` (exactly today's
  behavior), **or** when any single cross-exemplar pair reaches
  `exemplar_merge_threshold` (default **0.70**). One strong pair is
  decisive by design: spread-deduplication keeps at most one exemplar per
  look per cluster, so a genuinely shared look yields exactly one pair —
  requiring two would block the common case — while the 0.70 bar sits
  above every different-person cosine live calibration measured (≤ 0.51)
  and inside the same-person range (0.67–0.83).
All thresholds live in `FaceClusterer::Options` like the FACE-2 ones and are
calibrated live the same way (§5 of todo_face_recognition.md).

### 2.3 FACE-7c — ambiguity margin at join time

Today a face joins the single best cluster if it clears `join_threshold` —
even when the runner-up is 0.005 behind. `assign()` now tracks best and
second-best cluster scores; a face joins only when
`best >= join_threshold` **and** `best - second_best >= 0.05`
(`join_margin`; the margin test is skipped when there is no second cluster).
An ambiguous face stays unclustered — the catch-up retries it every tick, so
it joins naturally once the competing clusters sharpen or merge. Faces
already assigned are untouched (the margin gates joins, it never evicts).
`join_margin = 0` restores pre-FACE-7 behavior; hermetic tests for old
behavior set it explicitly.

### 2.4 FACE-7d — outlier review in the face-stack popup

False positives are surfaced, never auto-removed — FACE-3b's "user
constraints win" holds. `ListClusterFaces` gains a per-face
`fit` (double): the face's score (§2.2 definition) against its own cluster
**excluding itself** from the exemplar set (a face must not vouch for
itself). Cost: ≤ 500 listed faces × ≤ 6 vectors — computed on demand at
popup open, embeddings read for the listed page only.

Client (FACE-6 popup): faces with `fit < join_threshold − 0.05`
(`suspect_slack`, server-side constant; the flag ships as a `bool suspect`
so the client stays dumb) get an amber ring and a tooltip
("may not be ⟨name⟩ — right-click to remove"); the grid order stays
quality-desc. The pinned cover is never flagged (it is the anchor). One
right-click on a flagged face runs the existing FACE-3b removal; the next
open re-scores.

## 3. Schema & proto

```sql
CREATE TABLE IF NOT EXISTS face_cluster_exemplars (
  cluster_id INTEGER NOT NULL,
  face_id INTEGER NOT NULL,
  PRIMARY KEY (cluster_id, face_id)
) WITHOUT ROWID
```
(in `ensureTable`'s migration list, same idempotent pattern).

```proto
// FACE-7d: ClusterFaceInfo gains
double fit = 8;      // score against own cluster, self-excluded
bool suspect = 9;    // fit below the review bar; pinned cover never suspect
```
No new RPCs. `NameFaceCluster` behavior extends server-side (7a) with no
proto change.

## 4. Tests (hermetic, synthetic unit vectors, house pattern)

- 7a: name two clusters the same ⇒ one cluster remains, faces moved, person
  intact; a cannot-link against the dissolved source now points at the
  survivor (regression for the repoint gap); pinned source cover survives
  per FACE-6 rules.
- 7b: selection — quality order, spread bar, K cap, pinned face always
  present; scoring — a face matching a secondary exemplar but not the
  centroid joins; consolidation — same-mode pair merges via exemplar
  agreement while a single-noisy-pair stranger does not (2-pair rule);
  merged cluster re-selects exemplars covering both modes.
- 7c: two near clusters ⇒ ambiguous face stays unclustered; after the
  clusters merge, the same face joins; margin 0 restores old behavior
  (existing FACE-2 tests updated to pin margin 0 where they construct
  deliberate near-ties).
- 7d: an off-identity face in a cluster gets `suspect = true`, on-identity
  faces do not; the pinned cover is never suspect; `fit` excludes self.

## 5. Manual verification

1. Name a second cluster with an existing name ⇒ cards collapse into one;
   search still finds all photos; aliases intact.
2. A person with two visual eras: after 7b lands and a consolidation pass
   runs, new photos of either era join the one cluster.
3. Pin a cover, then sweep new photos ⇒ matches at least as good as before
   (anchor in the match set).
4. Open a big cluster's popup ⇒ obviously-wrong faces carry the amber ring;
   removing them clears the ring on reopen.
5. Watch a sweep tick log: unclustered-by-margin faces reported, and their
   count drains as clusters consolidate.

## 6. Implementation order

1. 7a: same-name merge in the handler + `repointCannotLinks` in
   `mergeClusters` + tests.
2. 7b schema + exemplar selection + clusterer load/scoring + consolidation
   two-stage test + tests.
3. 7c margin + tests (pin margin 0 in affected existing tests).
4. 7d fit/suspect on `ListClusterFaces` + popup ring + stub regen + tests.
5. §5 manual pass on the live library; calibrate `exemplar_spread`,
   `merge_prefilter`, `join_margin` against real cosines as FACE-2 did.
