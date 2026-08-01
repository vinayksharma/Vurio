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

---

## Hover scrollbars on horizontal strips (2026-07-31)

All three horizontal thumbnail strips (viewer strip, VU day panels, BINNED candidate rows) show a custom overlay scrollbar that fades in on hover and out on exit. Unlike the native indicator, its thumb is sized and positioned against the strip's FULL logical content — including pages not yet lazily loaded (a day's total file count; the timeline's total day count, with days before the loaded window shifting the thumb) — so the thumb honestly reflects where you are in everything that can load, with unloaded units estimated at the average loaded-unit width. Implemented as a single reusable modifier in `HoverScrollbar.swift` (`hoverHorizontalScrollbar` on the ScrollView) that introspects the backing NSScrollView for offset/content/viewport geometry — SwiftUI preference-based measurement reports zero width from inside macOS ScrollView content, so the AppKit clip/document views are observed directly (same pattern as VurioScroller). Binding only considers NSScrollViews whose window-space frame coincides with the modifier's own background view (they are geometrically identical by construction) — this pins each bar to its own strip; without it a bar could bind a sibling strip's scroller and drag two strips at once, or capture a vertical scroller in layered hierarchies like the embedded viewer. The bar is interactive: clicking the track jumps there and the same gesture drags; the interactive band is itself an AppKit view (a SwiftUI gesture would lose hit-testing to the NSScrollView beneath) that tracks its own hover via NSTrackingArea and only hit-tests while visible, so tile clicks are unaffected when hidden. Dragging past the loaded region clamps to the loaded edge and extends as sentinels page more content in. The bar never shows when the content fits the viewport.

---

## Viewer image fade transitions (2026-07-31)

Changing the displayed image in the embedded viewer fades rather than snaps: navigating cross-fades directly between images when the next preview is already cached, and otherwise fades the old image out (spinner fades in) and the new one in when it arrives (0.18s ease-in-out). A zoomed/panned image glides back to fit during the fade instead of snapping first. Error/unreadable placeholders fade the same way. The full-resolution swap is deliberately excluded — same image identity, so it remains an instant in-place sharpening.

---

## Full-resolution on zoom (2026-07-31)

When the user zooms past 1.2× in the embedded viewer (same threshold as the legacy web viewer), the app seamlessly upgrades the displayed image from the QHD preview to full resolution: the preview stays on screen until the full-res bitmap is decoded, then swaps in place with all transforms (zoom/pan/rotation) preserved — the fitted frame is identical for both bitmaps, so the only visible change is added sharpness.

- **Source (hybrid):** standard formats (JPEG/PNG/HEIC/TIFF…) are decoded directly from the original file on disk — byte-identical full resolution, no server hop. RAW formats (and standard files the local decoder can't read) go through `GetPreview(size: "full")`: the server generates a native-resolution JPEG (quality 95, embedded-JPEG extraction for RAW) asynchronously, indexed in a session-lifetime temp cache, announced via the `SubscribeToMediaReady` push (`media_type: "preview_full"`).
- One request per image (in-flight lock); failures unlock so the next zoom retries; `FAILED_PRECONDITION` (unreadable file) keeps the preview and never retries. Selection changes reset the state.
- Server-side full-res generation runs as task type `fullres_generator`, counted under the same `tpm.mediaGeneration.maxConcurrent` cap as thumbnails/previews.
- While the full-res image is being pulled, a small non-interactive "LOADING FULL RES" capsule (spinner + DSEG-style caption) fades in at the bottom-right of the image area and fades out when the swap completes or the request ends.
- **Best-available serving:** once a full-resolution image is known to be cheap — the server has it materialized in its session cache, the file is a standard format served as its original path, or a local decode already succeeded — subsequent views of that image serve full resolution directly and skip the QHD step and the zoom-time upgrade entirely. `MediaPathResponse.is_full_resolution` tells the client what it received; the client keeps a session-scoped set of such paths and falls back to the normal preview flow if the full-res source disappears (server restart, unmounted volume).

---

## Unreadable-media state (2026-07-31)

When the server negative-caches a file after repeated thumbnail/preview generation failures (gRPC `FAILED_PRECONDITION`, e.g. a corrupted RAF with a zeroed header), the UI shows a distinct state instead of the generic error and stops re-requesting: grid tiles render a warning triangle + "Unreadable" with the reason in the tooltip; the viewer's main area shows "File unreadable — unsupported or corrupted" plus the server's reason. Thumbnails/previews still generating are awaited via the `SubscribeToMediaReady` push stream (with polling fallback) up to 60 s instead of the old ~4 s retry budget.

### Implementation notes

- The `ViewerState` handoff singleton is gone; the double-click handlers in `VurioDuplicatesView` set a per-tab `ViewerRequest` (`vuViewerRequest` / `binnedViewerRequest`) directly, and `ViewerView` takes the request as a prop plus an `onClose` callback.
- Both `ViewerView` instances stay mounted inside `VurioDuplicatesView`: a closed viewer parks offscreen to the right (offset), open/close is animated at the mutation sites, and tab switches swap the instances instantly via opacity — the same always-mounted pattern the two scrollers use.
- Viewer keyboard shortcuts (now including Esc) are gated on the instance being the slid-in viewer of the frontmost tab (`isFrontmost` is passed down from `ContentView` so shortcuts stay dead while SETTINGS/SCROLLER are shown).
