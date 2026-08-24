# FACE-8 — configurable purge cutoff: merges filter misfits, live from Settings

Status: implemented 2026-08-24 (config key + live subscription, purge op,
merge wiring, purged-count RPC surfacing, Settings slider, unit tests);
remaining before Done: the §7 manual pass on the live library.

Depends on: FACE-6 (pinned covers), FACE-7 (exemplars, fit/suspect scoring,
same-name merge).

## 1. Motivation

Combining two clusters of one person (FACE-7a same-name merge, or a manual
Cmd-click merge) makes the identity model stronger — the combined exemplars
cover more looks — but any false positives that squeaked into either group
ride along. FACE-7d only *flags* them (amber ring, `suspect`); removal was
manual. This task closes the loop: **a merge filters the combined
membership through a fit cutoff**, so accuracy improves and false positives
leave in the same act. The cutoff is a Settings-panel value that takes
effect live — changing it queues a one-shot purge sweep over every cluster
(the accepted "re-binning" side effect), no server restart.

## 2. Semantics

### 2.1 One knob (decision: unify)

`media.face.mergePurgeCutoff` (double, default **0.53** = the old
`kSuspectFitBar`) drives BOTH:

- the popup's amber suspect ring (`ListClusterFaces` → `fit`/`suspect`), and
- the purge: what's flagged is exactly what a merge would set aside.

`0` disables purging — and, consistently, the ring never shows.

### 2.2 The purge (`FacesOps::purgeMisfits(db, cluster_id, cutoff)`)

- Fit = the FACE-7d score: max cosine against the cluster's centroid and
  exemplars, **self-excluded** (a face must not vouch for itself).
- Exemplars are refreshed *first*, so a just-merged cluster judges faces by
  its combined model, not the stale pre-merge set.
- Faces below the cutoff are **detached back to the unclustered pool**:
  embeddings kept, `cluster_id`/`person_id` cleared, **no cannot-link** — a
  statistical judgment, not a human verdict, so the face may return if the
  cluster's model shifts. (Contrast "Not ⟨name⟩", which is a verdict and
  does record the constraint.)
- The **pinned cover is exempt** — the human-verified anchor, matching
  FACE-7d's "pinned is never suspect".
- One pass, then one `recomputeCluster` (which re-refreshes exemplars and
  deletes an emptied cluster). No fixed-point loop: iterating shifts the
  centroid each round and can cascade into ejecting a minority look.
- No oscillation by construction: joining needs `join_threshold` (0.58)
  against the same scoring function, so a face shed at < 0.53 cannot
  bounce straight back; it stays pooled or finds its true home.

### 2.3 Where it runs (decision: all merges)

- `mergeClusters(db, target, sources, purge_cutoff, &purged)` — purge runs
  after the merge completes, covering BOTH the FACE-7a same-name path
  (`mergePersonClusters` passes through) and manual Cmd-click merges.
- The config-change sweep (§4).
- NOT on `consolidate()`'s automatic merges — the sweep's own merges are
  threshold-gated already; purging there would silently churn membership
  every tick.

## 3. Config plumbing

- `config/config.schema.json`: `media.face.mergePurgeCutoff` (double,
  0.53) and `media.face.enabled` (bool, false — previously only
  auto-registered from config.yaml, which made the Settings row read
  "Loaded from file").
- `config/config.descriptions.json`: "Face Match Strictness" + AST long
  description quoting the live calibration (different ≤ 0.51, same ≥ 0.67).
- Registration is load-bearing: an unregistered key is invisible to
  `GetConfig`/`UpdateConfig` and never renders in the panel.
- The other face keys (`joinThreshold` etc.) stay unregistered by decision:
  calibrated internals, startup-read only.

## 4. Live adaptation (eventing)

- RPC merge paths read the cutoff per call:
  `DuplicatesServiceImpl::faceFitCutoff()` →
  `config_->getPropertyValue<double>(...)` (service now takes the config
  manager; null in tests ⇒ `kSuspectFitBar`).
- Already-clustered faces: `ServerInitializer` subscribes to config
  changes (the duplicate-finder-interval pattern); on
  `media.face.mergePurgeCutoff` it latches the new value into
  `FaceScanService::requestPurgeSweep(cutoff)` — an atomic, because the
  event fires on a gRPC thread and the clusterer is single-threaded by
  contract. The next tick consumes the latch, runs `purgeMisfits` over
  every cluster **before** the mirror reload, and the same sweep's
  catch-up re-homes the shed faces.
- End-to-end: Settings → Save Changes → `UpdateConfig` persists to YAML +
  emits the event → within one sweep interval (15 s) the library is
  re-filtered. No restart.

## 5. RPC surface

`FaceClusterActionResponse` gains `int32 purged_faces = 3` (set by
`NameFaceCluster` and `MergeFaceClusters`). The client surfaces it —
"N faces didn't match well and were set aside for re-grouping" — because a
silently shrinking count reads as photos vanishing.

## 6. Client (Settings panel)

`FaceCutoffSliderSection` (decision: custom slider, not the generic numeric
row) under "Media – Face": single-thumb slider over 0.40–0.70 with
calibration markers ("strangers ≤ .51", "same person ≥ .67"), a
lenient→aggressive gradient track, a live value readout, and an on/off
toggle that writes `0` (purging disabled). Saves through the panel's normal
Save Changes flow; the key is excluded from the generic rows.

## 6b. FACE-8b: "Clear All Faces" (Settings → Advanced)

A destructive maintenance action beside Reset Duplicates: factory-reset the
people layer. `AdvancedSettingsService.ClearAllFaceGroups` →
`FacesOps::clearAllFaceGroups`: detach every face (embeddings kept), delete
all clusters, exemplars, cannot-links, persons and aliases — dissolveCluster,
globally. The sweep's catch-up regroups everything into fresh unnamed
clusters within ticks; photos and detected faces are untouched. The UI is a
confirm-guarded destructive ActionRow ("names cannot be recovered").

## 7. Manual verification

1. Settings shows "Media – Face" with the Face Recognition toggle (proper
   label) and the strictness slider at 0.53.
2. Name two clusters the same, confirm the combine ⇒ merged group; if any
   faces fell below the bar, the message bar reports the count and the
   shed faces reappear in other/new groups within a sweep or two.
3. Raise the cutoff to ~0.60, Save Changes ⇒ server log shows "purge sweep
   queued" then "Face purge sweep: cutoff 0.60, N face(s) set aside";
   PEOPLE tab counts drop accordingly; no restart.
4. Toggle strictness off (writes 0) ⇒ no amber rings in any popup; merges
   purge nothing.
5. Pin a below-bar face as cover ⇒ it survives merges and sweeps.
6. Advanced → Clear All Faces ⇒ PEOPLE tab empties, then repopulates with
   unnamed groups over the following sweep ticks; search no longer resolves
   the old names/aliases.
