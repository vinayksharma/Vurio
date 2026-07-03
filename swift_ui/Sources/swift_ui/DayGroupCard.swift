import SwiftUI
import AppKit

/// One row of the day-based "ALL" view: every photo taken on one calendar day,
/// sorted left-to-right by taken time, with a DSEG7 date stamp in the top-left.
/// Files that belong to a duplicate group collapse into a stack; tapping the
/// stack expands it inline into a bordered set (that day's members), tapping the
/// collapse button folds it back.
struct DayGroupCard: View {
    let day: Vurio_Dedup_DayGroup
    var duplicateTightness: Double
    var isEjecting: Bool
    var isSelected: Bool
    var onThumbnailTapped: (Vurio_Dedup_DuplicateGroup, Int) -> Void
    var onEjectSet: ([String]) -> Void
    var onRestoreSet: ([String]) -> Void
    var onLocateSet: (String) -> Void

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
        var result: [DayItem] = []
        var seenGroups: Set<String> = []
        for file in day.files {
            if file.groupID.isEmpty {
                result.append(.single(file))
            } else if !seenGroups.contains(file.groupID) {
                seenGroups.insert(file.groupID)
                let members = day.files.filter { $0.groupID == file.groupID }
                result.append(.stack(file.groupID, members))
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
        DuplicateMemberView(file: asDuplicateFile(file), isRep: isRep, isEjecting: isEjecting, onRestore: {
            onRestoreSet([file.filePath])
        }, onEject: {
            onEjectSet([file.filePath])
        }, onThumbnailTapped: {
            if !isEjecting {
                let index = popupFiles.firstIndex(where: { $0.filePath == file.filePath }) ?? 0
                onThumbnailTapped(makeGroup(id: popupGroupID, files: popupFiles), index)
            }
        }, onLocate: {
            onLocateSet(file.filePath)
        })
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
                    ThumbnailView(filePath: file.filePath, size: CGSize(width: 70, height: 70), cachedThumbnailPath: file.cachedThumbnailPath)
                        .frame(width: 70, height: 70)
                        .cornerRadius(8)
                        .overlay(
                            RoundedRectangle(cornerRadius: 8)
                                .stroke(index == 0 ? Theme.error : Theme.border, lineWidth: index == 0 ? 1.5 : 1)
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
                    .background(Capsule().fill(Theme.error.opacity(0.85)))
                    .offset(x: 6, y: 6)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("\(members.count) duplicates taken this day — click to expand")
        .opacity(top.isEjected ? 0.85 : 1.0)
    }

    /// Expanded duplicate group: this day's members side by side inside a red
    /// bordered set, with the binned view's bracket action underneath (eject the
    /// slider-eligible members, or restore when they are all ejected).
    @ViewBuilder
    private func expandedStackView(groupID: String, members: [Vurio_Dedup_DayFile]) -> some View {
        let eligible = members.filter { file in
            let score = file.similarityScore > 0 ? file.similarityScore : 1.0
            return !file.isRepresentative && (1.0 - score) <= duplicateTightness
        }
        let toRestore = eligible.filter { $0.isEjected }.map { $0.filePath }
        let toEject = eligible.filter { !$0.isEjected }.map { $0.filePath }
        let isRestoreMode = toEject.isEmpty && !toRestore.isEmpty
        let setFill: Color = Theme.error.opacity(0.06)
        let setStroke: Color = Theme.error.opacity(0.8)

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
        .overlay(alignment: .topTrailing) {
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
                            .fill(Theme.error.opacity(0.85))
                            .shadow(color: .black.opacity(0.6), radius: 3, x: 0, y: 2)
                    )
            }
            .buttonStyle(.plain)
            .offset(x: 7, y: -7)
            .help("Collapse this duplicate set")
        }
        .overlay(alignment: .bottom) {
            if !toEject.isEmpty || !toRestore.isEmpty {
                BracketView(
                    width: CGFloat(members.count) * 70 + CGFloat(max(0, members.count - 1)) * 12 + 8,
                    isRestoreMode: isRestoreMode,
                    isEjecting: isEjecting,
                    action: {
                        if isRestoreMode {
                            onRestoreSet(toRestore)
                        } else {
                            onEjectSet(toEject)
                        }
                    }
                )
                .offset(y: 16)
            }
        }
    }
}
