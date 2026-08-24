# FACE-6 — Face-Stack Popup: click-to-open, inline rename, lens search, pick-the-cover

Status: implemented 2026-08-24 (schema, pin plumbing through all five
election sites, `SetFaceClusterCover` RPC, popup rework, §5 unit tests);
remaining before Done: the §6 manual pass on the live library.

Manual-pass fixes (2026-08-24): the header shows the akas under the name
in brackets, and the alias editor is seeded when the chevron opens it (the
old open-popup-time seeding went stale on the second open); a name-only
Enter with the editor closed re-sends the cluster's current aliases, so
they still can't be wiped. `FaceCropView` re-crops when the bbox changes,
not just the file — picking a new cover face in the same photo now updates
the card. Renames are optimistic: the card/popup update instantly, the RPC
reconciles in the background, and rapid successive edits coalesce into one
in-flight RPC per cluster (latest value wins; failure reloads the server's
truth).
Depends on: FACE-3 (PEOPLE tab), FACE-3b (cannot-links), FACE-5 (aliases).

## 1. Motivation

The PEOPLE tab currently splits one person's interactions across three
surfaces: single-click drills into VU, the name/alias dialog hides behind a
context menu, and the per-face review grid hides behind a second context-menu
item. The cover face is chosen purely by detection quality and cannot be
changed. This task makes the face stack the person's home: a single click
opens it, and renaming, searching, choosing the representative face, and
detaching wrong faces all live inside it.

## 2. UX specification

### 2.1 Opening

- **Single click** on a cluster card opens the face-stack popup for that
  cluster. This replaces the current single-click behavior (VU drill-down),
  which moves to the lens icon (§2.2).
- **Cmd-click** still toggles the merge selection — unchanged.
- The card's context menu keeps **Name…/Rename…**, **Merge**, and
  **Hide/Unhide**. The **"Review faces…"** item is removed: the popup *is*
  the review grid now (decision: replace, not coexist).
- Presentation: the existing `reviewSheet` sheet (`minWidth 520 × minHeight
  420`) is reworked in place — same `model.reviewing` /
  `model.openReview(_:)` plumbing, new header and gestures. A cluster can
  hold up to 500 listed faces (the `ListClusterFaces` limit), so a sheet,
  not an anchored popover.

### 2.2 Popup header

Left to right:

1. **Inline name field.** A borderless `TextField` seeded with the cluster
   name, placeholder "Unnamed". **Enter commits** via the existing
   `NameFaceClusterRequest` (empty text clears the name). Losing focus
   without Enter reverts — no silent saves.
2. **Aliases disclosure.** A small chevron ("aka…") expands a second field
   seeded with the FACE-5 aliases, comma-separated, committed on Enter
   together with the name. `NameFaceClusterRequest.aliases` *replaces* the
   alias set, so the field must always be sent pre-filled — committing the
   name with an untouched alias field must not wipe aliases.
3. **Lens icon** (`magnifyingglass`, small, `Theme.textSecondary`, hover
   highlight). Clicking it publishes the cluster to
   `PeopleNavigator.shared.pending` — exactly what card-click does today —
   then closes the popup. Tooltip: "Show this person's photos".
4. Spacer, then the existing **Done** button.

The "right-click a face that isn't them" caption stays, extended: "click a
face to make it the cover — right-click one that isn't them".

### 2.3 Face grid interactions

The grid itself (adaptive 84–100pt `FaceCropView` tiles, quality-desc order)
is unchanged. New gestures per tile:

- **Left click** → this face becomes the cluster's **representative (cover)
  face**: call the new `SetFaceClusterCover` RPC (§3), then close the popup.
  The card's circle crop updates on the reload that every action already
  triggers. The current cover face gets a subtle ring
  (`Theme.accent`, 2pt) so the user can see which face is the cover today.
- **Right click** → context menu with one destructive item:
  - unnamed cluster: **"Not the same person"**
  - named cluster: **"Not ⟨name⟩"** (e.g. "Not Alice")
  Behavior is the existing FACE-3b `RemoveFaceFromCluster`: detach, record
  the cannot-link, let the sweep re-home the face. Label only — no
  behavioral change. (The label uses the *canonical* name, never an alias.)
- If the removed face was the pinned cover, the server unpins and re-elects
  (§3.3); the client just reloads.

## 3. Backend: pinned covers

### 3.1 Schema

```sql
ALTER TABLE face_clusters ADD COLUMN cover_pinned INTEGER NOT NULL DEFAULT 0
```

Added in `faces_ops.cpp` `ensureTables` alongside the existing FACE-2
`cover_quality` migration (same idempotent ALTER-and-swallow pattern).

### 3.2 Proto + RPC

```proto
// FACE-6: pin a specific face as the cluster's representative (cover) face.
// A pinned cover survives automatic quality-based re-election permanently;
// it unpins only when that face leaves the cluster.
message SetFaceClusterCoverRequest {
  int32 cluster_id = 1;
  int32 face_id = 2;
}
rpc SetFaceClusterCover(SetFaceClusterCoverRequest) returns (FaceClusterActionResponse);
```

Handler validates that `face_id` currently belongs to `cluster_id`
(`success=false, "face is not in this cluster"` otherwise — the popup may
race the sweep re-homing a face), then sets `cover_face_id`,
`cover_quality` (from the face row, kept accurate for merge comparisons),
and `cover_pinned = 1`.

Optionally expose `bool cover_pinned = 12;` on `FaceClusterInfo` and
`bool is_cover` derivable client-side (the client already knows
`cover_face_id`? — it does not; add `int32 cover_face_id = 13;` to
`FaceClusterInfo` so the grid can ring the current cover, pinned or not).

### 3.3 Election sites — pin wins everywhere

Every place that writes `cover_face_id` automatically must now respect the
pin. Enumerated (all current sites as of FACE-3b):

| Site | Current behavior | With pin |
|---|---|---|
| `face_clusterer.cpp:111` incremental join | new face's quality > cover_quality ⇒ take over | skip when `cover_pinned` |
| `face_clusterer.cpp:127` new cluster | first face is cover | unchanged (nothing to pin yet) |
| `face_clusterer.cpp:201` in-memory merge | higher `cover_quality` wins | pinned beats unpinned; both pinned ⇒ **target's** pin wins; both unpinned ⇒ quality as today |
| `faces_ops.cpp:554` DB merge | same | same rule |
| `faces_ops.cpp:842` `recomputeCluster` election | max-quality member | if `cover_pinned` **and** the pinned `cover_face_id` is still a member ⇒ keep it; if it is gone ⇒ clear the pin and elect by quality |

`recomputeCluster` runs after `RemoveFaceFromCluster`, so "removed face was
the cover" needs no special-casing in the handler — the recompute's
membership check unpins naturally. The clusterer's in-memory `Cluster`
struct and `ClusterRow` both gain a `cover_pinned` field, loaded/saved with
the existing cover fields (`face_clusterer.cpp:51/75`, `faces_ops.cpp`
load/insert/update statements).

### 3.4 Merge semantics (explicit)

Pin propagates through merges: merging B into A where B's cover is pinned
and A's is not ⇒ A shows B's pinned cover, still pinned. Both pinned ⇒ A
(the merge target the user chose) keeps its own. This matches user intent:
a pin is a statement about the *person*, not the row id.

## 4. Client changes (`swift_ui/Sources/swift_ui/PeopleView.swift`)

- `clusterCard`: `onTapGesture` → `Task { await model.openReview(cluster) }`
  (was: publish to `PeopleNavigator`). Cmd-click gesture untouched.
  Context menu: drop "Review faces…".
- `reviewSheet` header per §2.2. The lens action is the old click body:
  publish `ClusterFilter(id:name:)`, then `model.reviewing = nil`.
- Inline rename state lives in the sheet (`@State renameText/aliasText`,
  seeded in `openReview`); commit calls the existing `model.rename`.
  The old alert stays — the card context menu still uses it.
- Face tile: `onTapGesture` → `model.setCover(faceID)` (new VM method,
  `action`-wrapped like the others, then `reviewing = nil`); `contextMenu`
  label becomes `cluster.name.isEmpty ? "Not the same person" : "Not \(name)"`.
- Cover ring: compare `face.faceID == cluster.coverFaceID` (new proto field).
- Regenerate Swift protos (`Generated/`).

## 5. Tests

- `faces_ops` unit tests: pin then insert a higher-quality face ⇒ cover
  unchanged; pin then merge (pinned←unpinned, unpinned←pinned,
  pinned←pinned both directions) ⇒ table in §3.3; pin then remove the
  pinned face ⇒ recompute unpins and elects max-quality; `SetFaceClusterCover`
  with a face from another cluster ⇒ `success=false`.
- Existing FACE-2/3 clustering tests must stay green (default
  `cover_pinned=0` ⇒ behavior identical).

## 6. Manual verification

1. Click a cluster card ⇒ popup opens; Cmd-click still selects for merge.
2. Rename inline (Enter) ⇒ card label updates; aliases survive a
   name-only edit; search finds the person by the new name and by alias.
3. Lens ⇒ VU tab opens filtered to the person, popup closed.
4. Click a mediocre face ⇒ it becomes the card's cover, popup closed;
   run the sweep over new photos of that person ⇒ cover does not change.
5. Right-click ⇒ label reads "Not the same person" pre-naming and
   "Not ⟨name⟩" after; removing the pinned cover ⇒ card falls back to the
   best-quality face.
6. Merge a pinned cluster into an unpinned one ⇒ pinned face survives.

## 7. Implementation order

1. Schema + `faces_ops` pin plumbing + election-site changes + unit tests.
2. Proto (`SetFaceClusterCoverRequest`, `cover_face_id`/`cover_pinned` on
   `FaceClusterInfo`) + handler + regenerate C++/Swift stubs.
3. SwiftUI: popup rework (header, gestures, labels), card click change.
4. §6 manual pass on the live library.
