import SwiftUI
import AppKit

/// One row of the day-based "ALL" view: every photo taken on one calendar day,
/// sorted left-to-right by taken time, with a DSEG7 date stamp in the top-left.
/// Files that belong to a duplicate group collapse into a stack; tapping the
/// stack expands it inline into a bordered set (that day's members), tapping the
/// collapse button folds it back.
struct DayGroupCard: View {
    let day: Vurio_Dedup_DayGroup
    var isEjecting: Bool
    var isSelected: Bool
    // Path of the file selected by single-click (accent border on its tile).
    var selectedFilePath: String?
    // Single-click selects the file; double-click opens it in the VIEWER tab.
    var onThumbnailTapped: (Vurio_Dedup_DayFile) -> Void
    var onThumbnailDoubleTapped: (Vurio_Dedup_DayFile) -> Void
    var onLocateSet: (String) -> Void
    // Called when the trailing sentinel scrolls into view and the day has more
    // files than are loaded (horizontal-scroll pagination).
    var onLoadMore: (() -> Void)? = nil
    // True while a search is active: render tiny ai/xf badges under each
    // tile's bottom-left corner (advanced overlay only). Which badge lights
    // up is per-file — the server unions the CLIP and EXIF halves and tags
    // every DayFile with matchedAi / matchedExif.
    var showSearchBadges: Bool = false

    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false

    @State private var expandedGroupIDs: Set<String> = []
    @State private var isCardHovered = false

    /// THUMB-1: every VU tile is this tall; widths follow each image's display
    /// geometry (ThumbnailGeometry). Row height stays fixed.
    static let tileHeight: CGFloat = 70

    /// Display items: unique files stay singles; a duplicate group's members
    /// (this day's only) coalesce into one stack at the earliest member's slot.
    private enum DayItem: Identifiable {
        case single(Vurio_Dedup_DayFile)
        case stack(String, [Vurio_Dedup_DayFile])

        var id: String {
            switch self {
            case .single(let file): return "file-\(file.id)"
            case .stack(let groupID, _): return "stack-\(groupID)"
            }
        }
    }

    private var items: [DayItem] {
        // Single pass: bucket group members first, then emit each single/stack at
        // its first member's slot. (The previous per-group `filter` was O(files²),
        // recomputed on every body evaluation — a real cost on multi-hundred-file
        // days, re-triggered by hover/expand state changes.)
        var membersByGroup: [String: [Vurio_Dedup_DayFile]] = [:]
        for file in day.files where !file.groupID.isEmpty {
            membersByGroup[file.groupID, default: []].append(file)
        }
        var result: [DayItem] = []
        var seenGroups: Set<String> = []
        for file in day.files {
            if file.groupID.isEmpty {
                result.append(.single(file))
            } else if seenGroups.insert(file.groupID).inserted {
                let members = membersByGroup[file.groupID] ?? [file]
                // A group whose other members were taken on different days (or
                // are not loaded yet) has only one member here — render it as a
                // plain single; a fanned "stack of 1" with a ×1 badge is noise.
                if members.count == 1 {
                    result.append(.single(file))
                } else {
                    result.append(.stack(file.groupID, members))
                }
            }
        }
        return result
    }

    /// "YYYY-MM-DD  HH:MM" — the day plus the first photo's taken time. Double
    /// space: DSEG7 renders a single space too narrow to read as a separator.
    private var dateStampText: String {
        if let first = day.files.first, first.takenTime.count >= 16 {
            let time = String(first.takenTime.dropFirst(11).prefix(5))
            return "\(day.day)  \(time)"
        }
        return day.day
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            DateStampLabel(text: dateStampText)

            VerticalScrollingFixView {
                // Native indicator off: the hover scrollbar below scales its
                // thumb to the day's TOTAL file count, not just the pages
                // loaded so far.
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(alignment: .center, spacing: 12) {
                        ForEach(items) { item in
                            switch item {
                            case .single(let file):
                                singleView(file)
                            case .stack(let groupID, let members):
                                if expandedGroupIDs.contains(groupID) {
                                    expandedStackView(groupID: groupID, members: members)
                                } else {
                                    collapsedStackView(groupID: groupID, members: members)
                                }
                            }
                        }

                        // Trailing sentinel: the LazyHStack materializes it only
                        // when scrolled near the end of the loaded files, which
                        // pages in the day's next chunk. Its id changes with the
                        // loaded count so each appended page re-arms onAppear.
                        if day.files.count < Int(day.totalFiles) {
                            ProgressView()
                                .frame(width: 70, height: 70)
                                .background(Theme.surface.opacity(0.5))
                                .cornerRadius(8)
                                .id("load-more-\(day.files.count)")
                                .onAppear { onLoadMore?() }
                        }
                    }
                    .padding(.top, 22)
                    .padding(.bottom, 18)
                    .padding(.leading, 6)
                    .padding(.trailing, 12)
                }
                .hoverHorizontalScrollbar(totalUnits: Int(day.totalFiles),
                                          loadedUnits: day.files.count)
            }
            .frame(height: 118)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(isSelected ? Theme.surfaceElevated.opacity(0.8) : (isCardHovered ? Color.primary.opacity(0.05) : Theme.surface.opacity(0.5)))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(isSelected ? Theme.textAccent : Theme.border, lineWidth: isSelected ? 2 : 1)
        )
        .onHover { isCardHovered = $0 }
        .padding(.bottom, 4)
    }

    /// Reuses the binned view's member tile (hover eject/restore, ejected and
    /// missing states) by mapping the DayFile onto the DuplicateFile shape.
    private func asDuplicateFile(_ file: Vurio_Dedup_DayFile) -> Vurio_Dedup_DuplicateFile {
        Vurio_Dedup_DuplicateFile.with {
            $0.id = file.id
            $0.filePath = file.filePath
            $0.sizeBytes = file.sizeBytes
            $0.isRepresentative = file.isRepresentative
            $0.similarityScore = file.similarityScore
            $0.isEjected = file.isEjected
            $0.cachedThumbnailPath = file.cachedThumbnailPath
            $0.isAvailable = file.isAvailable
            // THUMB-1: without these the VU tiles silently fall back to square.
            $0.displayWidth = file.displayWidth
            $0.displayHeight = file.displayHeight
        }
    }

    @ViewBuilder
    private func singleView(_ file: Vurio_Dedup_DayFile) -> some View {
        memberView(file, isRep: false)
    }

    @ViewBuilder
    private func memberView(_ file: Vurio_Dedup_DayFile, isRep: Bool) -> some View {
        // VU is read-only for eject (allowEject: false) — no per-tile eject/restore
        // and no ejected (red) border. Eject management lives on the BINNED tab.
        DuplicateMemberView(file: asDuplicateFile(file), isRep: isRep, isEjecting: isEjecting, onThumbnailTapped: {
            if !isEjecting { onThumbnailTapped(file) }
        }, onThumbnailDoubleTapped: {
            if !isEjecting { onThumbnailDoubleTapped(file) }
        }, onLocate: {
            onLocateSet(file.filePath)
        }, isFocusedCandidate: file.filePath == selectedFilePath, allowEject: false, fixedSize: Self.tileHeight, showMatchScore: false)
        .overlay(alignment: .bottomLeading) { searchBadgeStrip(for: file) }
    }

    /// Advanced-overlay "ai"/"xf" badges under a tile's bottom-left corner:
    /// which search half admitted THIS file (results are a union of CLIP
    /// semantic and EXIF constraint matches, so tiles differ). Lit = that
    /// half matched, dimmed = it didn't. Drawn in the strip's bottom padding
    /// via offset, so the row height is unchanged.
    @ViewBuilder
    private func searchBadgeStrip(for file: Vurio_Dedup_DayFile) -> some View {
        if advancedOverlayEnabled, showSearchBadges {
            HStack(spacing: 3) {
                searchBadge("ai", enabled: file.matchedAi)
                searchBadge("xf", enabled: file.matchedExif)
            }
            .offset(y: 13)
            .allowsHitTesting(false)
        }
    }

    private func searchBadge(_ label: String, enabled: Bool) -> some View {
        Text(label)
            .font(.custom("Menlo", size: 7).weight(.bold))
            .foregroundColor(enabled ? Color.black.opacity(0.85) : Theme.textSecondary.opacity(0.55))
            .padding(.horizontal, 3)
            .padding(.vertical, 1)
            .background(
                RoundedRectangle(cornerRadius: 3)
                    .fill(enabled ? Theme.textAccent.opacity(0.9) : Theme.surface.opacity(0.7))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 3)
                    .stroke(enabled ? Color.clear : Theme.border.opacity(0.6), lineWidth: 0.5)
            )
            .help(label == "ai" ? "This image \(enabled ? "matched" : "did not match") the AI (semantic) search"
                                : "This image \(enabled ? "matched" : "did not match") the EXIF metadata search")
    }

    /// Collapsed duplicate group: up to three fanned thumbnails plus a count
    /// badge. Tapping expands the set inline.
    @ViewBuilder
    private func collapsedStackView(groupID: String, members: [Vurio_Dedup_DayFile]) -> some View {
        let top = members.first ?? members[0]
        // THUMB-1: every layer is laid out at its own display geometry, but the
        // stack's footprint in the strip is the TOP member's width — pinned with
        // .frame(width:) so a wider occluded layer cannot silently widen the
        // slot. (Members of one group are the same photo, so in practice the
        // layers share a shape; this only matters for rotated/cropped variants.)
        let topSize = ThumbnailGeometry.tileSize(height: Self.tileHeight,
                                                 displayWidth: top.displayWidth,
                                                 displayHeight: top.displayHeight)
        Button(action: {
            withAnimation(.spring(response: 0.3, dampingFraction: 0.75)) {
                _ = expandedGroupIDs.insert(groupID)
            }
        }) {
            ZStack {
                ForEach(Array(members.prefix(3).enumerated().reversed()), id: \.offset) { index, file in
                    // Layer priority = stack depth: the visible top (index 0) loads
                    // first; the occluded layers behind it wait until every stack's
                    // top has been delivered, then load one layer at a time.
                    let layerSize = ThumbnailGeometry.tileSize(height: Self.tileHeight,
                                                               displayWidth: file.displayWidth,
                                                               displayHeight: file.displayHeight)
                    ThumbnailView(filePath: file.filePath, size: layerSize, cachedThumbnailPath: file.cachedThumbnailPath, layerPriority: index, requestSize: ServerImageLoader.tileAssetSize)
                        .frame(width: layerSize.width, height: layerSize.height)
                        .cornerRadius(8)
                        .overlay(
                            RoundedRectangle(cornerRadius: 8)
                                .stroke(index == 0 ? Theme.success : Theme.border, lineWidth: index == 0 ? 1.5 : 1)
                        )
                        .opacity(file.isEjected && index == 0 ? 0.3 : 1.0)
                        .rotationEffect(.degrees(Double(index) * 3))
                        .offset(x: CGFloat(index) * 4, y: CGFloat(index) * -2)
                }
            }
            .frame(width: topSize.width, height: topSize.height)
            .overlay(alignment: .bottomLeading) { searchBadgeStrip(for: top) }
            .overlay(alignment: .bottomTrailing) {
                Text("\u{00D7}\(members.count)")
                    .font(.custom("Menlo", size: 9))
                    .fontWeight(.bold)
                    .foregroundColor(Theme.textPrimary)
                    .padding(.horizontal, 4)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(Theme.success.opacity(0.85)))
                    .offset(x: 6, y: 6)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("\(members.count) duplicates taken this day — click to expand")
        .opacity(top.isEjected ? 0.85 : 1.0)
    }

    /// Expanded duplicate group: this day's members side by side inside a green
    /// bordered set. VU is read-only for eject, so there is no bracket action here —
    /// eject/restore lives on the BINNED tab.
    @ViewBuilder
    private func expandedStackView(groupID: String, members: [Vurio_Dedup_DayFile]) -> some View {
        let setFill: Color = Theme.success.opacity(0.06)
        let setStroke: Color = Theme.success.opacity(0.8)

        HStack(alignment: .center, spacing: 12) {
            ForEach(members, id: \.id) { file in
                memberView(file, isRep: file.isRepresentative)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 8)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .fill(setFill)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .stroke(setStroke, style: StrokeStyle(lineWidth: 1.5))
        )
        .overlay(alignment: .topLeading) {
            Button(action: {
                withAnimation(.spring(response: 0.3, dampingFraction: 0.75)) {
                    _ = expandedGroupIDs.remove(groupID)
                }
            }) {
                Image(systemName: "arrow.right.and.line.vertical.and.arrow.left")
                    .font(.system(size: 8, weight: .bold))
                    .foregroundColor(Theme.textPrimary)
                    .frame(width: 18, height: 18)
                    .background(
                        Circle()
                            .fill(Theme.success.opacity(0.85))
                            .shadow(color: .black.opacity(0.6), radius: 3, x: 0, y: 2)
                    )
            }
            .buttonStyle(.plain)
            .offset(x: -7, y: -7)
            .help("Collapse this duplicate set")
        }
    }
}
