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
    var onThumbnailTapped: (Vurio_Dedup_DuplicateGroup, Int) -> Void
    var onLocateSet: (String) -> Void
    // Called when the trailing sentinel scrolls into view and the day has more
    // files than are loaded (horizontal-scroll pagination).
    var onLoadMore: (() -> Void)? = nil

    @State private var expandedGroupIDs: Set<String> = []
    @State private var isCardHovered = false

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

    /// "YYYY-MM-DD HH:MM" — the day plus the first photo's taken time.
    private var dateStampText: String {
        if let first = day.files.first, first.takenTime.count >= 16 {
            let time = String(first.takenTime.dropFirst(11).prefix(5))
            return "\(day.day) \(time)"
        }
        return day.day
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            DateStampLabel(text: dateStampText)

            VerticalScrollingFixView {
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
        }
    }

    /// Synthesizes a DuplicateGroup so the existing ImagePopupView can present
    /// any set of day files.
    private func makeGroup(id: String, files: [Vurio_Dedup_DayFile]) -> Vurio_Dedup_DuplicateGroup {
        Vurio_Dedup_DuplicateGroup.with {
            $0.groupID = id
            $0.files = files.map { asDuplicateFile($0) }
        }
    }

    @ViewBuilder
    private func singleView(_ file: Vurio_Dedup_DayFile) -> some View {
        memberView(file, isRep: false, popupFiles: [file], popupGroupID: "day-\(day.day)")
    }

    @ViewBuilder
    private func memberView(_ file: Vurio_Dedup_DayFile, isRep: Bool, popupFiles: [Vurio_Dedup_DayFile], popupGroupID: String) -> some View {
        // VU is read-only for eject (allowEject: false) — no per-tile eject/restore
        // and no ejected (red) border. Eject management lives on the BINNED tab.
        DuplicateMemberView(file: asDuplicateFile(file), isRep: isRep, isEjecting: isEjecting, onThumbnailTapped: {
            if !isEjecting {
                let index = popupFiles.firstIndex(where: { $0.filePath == file.filePath }) ?? 0
                onThumbnailTapped(makeGroup(id: popupGroupID, files: popupFiles), index)
            }
        }, onLocate: {
            onLocateSet(file.filePath)
        }, allowEject: false, fixedSize: 70, showMatchScore: false)
    }

    /// Collapsed duplicate group: up to three fanned thumbnails plus a count
    /// badge. Tapping expands the set inline.
    @ViewBuilder
    private func collapsedStackView(groupID: String, members: [Vurio_Dedup_DayFile]) -> some View {
        let top = members.first ?? members[0]
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
                    ThumbnailView(filePath: file.filePath, size: CGSize(width: 70, height: 70), cachedThumbnailPath: file.cachedThumbnailPath, layerPriority: index)
                        .frame(width: 70, height: 70)
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
                memberView(file, isRep: file.isRepresentative, popupFiles: members, popupGroupID: groupID)
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
