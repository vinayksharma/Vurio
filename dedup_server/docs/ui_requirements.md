# UI Requirements

Product requirements for the SwiftUI desktop frontend (`swift_ui/`). New UI requirements are appended to this file as dated sections.

## Overview

The SwiftUI app is the active Vurio frontend. It talks to the C++ `dedup_server` over gRPC and currently offers the sidebar tabs VU (day-based library view), BINNED (duplicate groups), SETTINGS, and SCROLLER (test harness).

---

## VIEWER Tab (2026-07-27)

### Summary

A new sidebar navigation tab named **VIEWER** hosts a full-client-area image viewing control composed of:

1. A **main image area** with basic, non-destructive image manipulation controls.
2. A **horizontally scrollable thumbnail strip** visually similar to a VU day panel, which can hold either a duplicate group's images or the complete, continuously scrollable timeline of every image in the library.

The VIEWER is reached by **double-clicking a thumbnail** in VU or BINNED. Alongside this, the click model of VU/BINNED changes: **single-click now selects a thumbnail; the full-screen `ImagePopupView` is retired** (double-click → VIEWER replaces it).

### Navigation & entry points

- New `SidebarItem.viewer` case; sidebar entry "VIEWER" placed between BINNED and SETTINGS.
- **From VU:** double-clicking any thumbnail (a single, or a member of an expanded duplicate stack) switches to VIEWER and loads the **library timeline** in the strip, scrolled to and selecting the double-clicked image.
- **From BINNED:** double-clicking any thumbnail (representative or candidate) switches to VIEWER and loads **that duplicate group** in the strip, selecting the double-clicked image.
- Selecting the VIEWER tab directly with no prior context shows an empty state ("Double-click an image in VU or BINNED").
- Missing files (disconnected volume, "Locate" mask) are not double-clickable, matching their single-click behavior.

### Click model change in VU and BINNED

- **Single click** on a thumbnail selects/highlights it (accent border, same treatment as the existing focused-candidate style) and selects its row. It no longer opens a popup.
- **Double click** opens the VIEWER as described above.
- `ImagePopupView` is removed. Eject/restore in BINNED remains available on the cards (hover buttons, bracket) and in the VIEWER (below).

### Main image area (manipulation controls)

Non-destructive view transforms only — nothing is written to disk:

- **Zoom**: in/out buttons, pinch/magnification gesture; clamped (0.1×–10×).
- **Pan**: drag the image when it exceeds the fitted size.
- **Rotate**: ±90° buttons (view-only).
- **Fit / reset**: reset zoom, pan, and rotation to aspect-fit.
- **Actual size**: 1:1 pixel zoom.
- Transforms reset when the selected image changes.
- Full-resolution preview is fetched via the existing `GetPreview` RPC (`ServerImageLoader.loadPreview`) with the shared `ImageCache`.
- **Keyboard**: ←/→ move the selection along the strip; shortcuts are active only while the VIEWER tab is frontmost.

### Thumbnail strip

Styled like a VU day panel (rounded card, 70 px tiles, horizontal scroll, DSEG7 date stamps), docked at the bottom of the VIEWER. Two content modes:

- **Timeline mode** (entered from VU): one flat, continuous horizontal strip of the entire library — days ordered newest→oldest (matching VU's vertical order), files within a day ascending by taken time, with a DSEG7 date-stamp separator at each day boundary. The strip loads a window of days around the anchor (the double-clicked image's day) via `GetDayGroups`/`GetDayFiles` and pages in more days in both directions as the user scrolls (leading/trailing sentinels). The library is never loaded whole.
- **Group mode** (entered from BINNED): the duplicate group's files in group order, representative first, ejected members shown dimmed.
- Clicking a strip tile selects it and loads it into the main image area; the selected tile shows the accent border and is kept scrolled into view.

### Eject / restore in VIEWER (group mode only)

- When the VIEWER was entered from BINNED, the toolbar exposes eject/restore for the selected image (plus Delete/Backspace shortcuts), mirroring the retired popup's behavior: representatives cannot be ejected; ejected files show the restore action.
- Actions call the existing `EjectDuplicates`/`UndoEjectDuplicates` RPCs; the strip tile and the BINNED/VU views update immediately (optimistic update through the existing view-models).
- In timeline mode the VIEWER is read-only, consistent with VU.

### Non-goals (this iteration)

- No destructive editing, adjustments (brightness/contrast), cropping, or saving.
- No video playback (library is photos-only).
- No group-stack coalescing in the timeline strip (duplicates appear as ordinary tiles).
- The retired popup's sheet-based quick look is not replaced by another quick-look affordance.

### Tab state retention (added 2026-07-30)

Switching between VU and BINNED must preserve each tab's view state — scroll position and row/thumbnail selection survive the switch in both directions. Implementation: both tabs' `VurioScroller`s stay mounted inside `VurioDuplicatesView` (opacity-toggled, the same pattern `ContentView` uses for its tab panels) instead of being swapped by an `if/else`, and each tab keeps its own selection state (`vuSelection` / `binnedSelection`).

### Implementation notes

- Tab switch + context handoff via a small shared `ViewerState` observable (`ViewerContent.timeline(anchorDayIndex:dayKey:file:)` / `.group(group:selectedIndex:)`).
- Double-click is implemented on the shared tile (`DuplicateMemberView`) as a `TapGesture(count: 2)` layered with the existing single tap; single-click select fires immediately (no double-click delay) since selection is a prerequisite of opening the viewer anyway.
- Eject-state changes made in the VIEWER are broadcast (NotificationCenter) so the persistent VU/BINNED views reconcile without a reload.

---

## Embedded Viewer (2026-07-30) — supersedes the VIEWER tab's navigation model

The separate VIEWER sidebar tab is removed; the viewer is now embedded in the VU and BINNED tabs themselves. Everything else about the viewer (main image area and transforms, thumbnail-strip modes, group-mode eject/restore, ←/→ and Delete shortcuts) is unchanged from the VIEWER Tab section above.

- **Open:** double-clicking a thumbnail slides that tab's grid out to the left and slides the viewer in from the right, in place (spring animation), filling the tab's client area.
- **Close:** an ✕ button at the top-right of the viewer's path bar — or Esc — slides the viewer out and the grid back in.
- **Two instances:** VU and BINNED each host their own viewer. An open viewer survives VU↔BINNED tab switches (consistent with the tab-state-retention rule): switching away and back returns to the viewer exactly as it was, until explicitly closed.
- **Sidebar** now offers VU, BINNED, SETTINGS, SCROLLER.

### Implementation notes

- The `ViewerState` handoff singleton is gone; the double-click handlers in `VurioDuplicatesView` set a per-tab `ViewerRequest` (`vuViewerRequest` / `binnedViewerRequest`) directly, and `ViewerView` takes the request as a prop plus an `onClose` callback.
- Both `ViewerView` instances stay mounted inside `VurioDuplicatesView`: a closed viewer parks offscreen to the right (offset), open/close is animated at the mutation sites, and tab switches swap the instances instantly via opacity — the same always-mounted pattern the two scrollers use.
- Viewer keyboard shortcuts (now including Esc) are gated on the instance being the slid-in viewer of the frontmost tab (`isFrontmost` is passed down from `ContentView` so shortcuts stay dead while SETTINGS/SCROLLER are shown).
