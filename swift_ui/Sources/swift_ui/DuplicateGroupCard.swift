import SwiftUI
import AppKit

struct DuplicateGroupCard: View {
    let group: Vurio_Dedup_DuplicateGroup
    var duplicateTightness: Double
    var isEjecting: Bool
    var isSelected: Bool
    @Binding var selectedCandidateIndex: Int
    var onThumbnailTapped: (Int) -> Void
    var onThumbnailDoubleTapped: (Int) -> Void
    var onEjectSet: ([String]) -> Void
    var onRestoreSet: ([String]) -> Void
    var onLocateSet: (String) -> Void
    // STRIP-1: this card's row in the BINNED list (drag anchoring).
    var rowIndex: Int = 0

    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    @ObservedObject private var strip = StripLayout.shared
    @State private var isRepEjectHovered = false
    @State private var isCardHovered = false
    // Path of the most recently hovered thumbnail in this group. Persists after
    // the pointer leaves until another thumbnail is hovered (advanced overlay).
    @State private var lastHoveredPath: String?
    
    var representative: Vurio_Dedup_DuplicateFile? {
        group.files.first(where: { $0.isRepresentative }) ?? group.files.first
    }
    
    var candidates: [Vurio_Dedup_DuplicateFile] {
        if group.files.contains(where: { $0.isRepresentative }) {
            return group.files.filter { !$0.isRepresentative }
        } else {
            return Array(group.files.dropFirst())
        }
    }

    /// Spacing of the candidate LazyHStack; the bracket geometry depends on it.
    private static let candidateSpacing: CGFloat = 12
    /// Leading padding of the candidate strip; the bracket geometry depends on it.
    private static let candidateLeadingPadding: CGFloat = 6

    /// THUMB-1 (§6.5): candidate tile widths follow each file's display
    /// geometry, so the eligibility bracket must be positioned by a prefix-sum
    /// over the ACTUAL widths — the old `index * (70 + 12)` arithmetic assumed
    /// every tile was 70 pt and would point the bracket (whose action ejects the
    /// files it spans) at the wrong photos.
    private var candidateWidths: [CGFloat] {
        candidates.map { DuplicateMemberView.tileSize(for: $0, isRep: false, fixedSize: nil, stripTileHeight: strip.tileHeight(for: .binned)).width }
    }

    /// prefix[i] = sum(widths[0..<i]) + i * spacing (leading x of tile i).
    private var candidatePrefixOffsets: [CGFloat] {
        ThumbnailGeometry.prefixOffsets(widths: candidateWidths, spacing: Self.candidateSpacing)
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top, spacing: 20) {
                if let rep = representative {
                    representativeMemberView(rep)
                }
                
                if !candidates.isEmpty {
                    Divider()
                        .frame(height: strip.tileHeight(for: .binned))
                        .background(Theme.borderActive)
                        .padding(.top, 30)
                        .padding(.bottom, 30)
                }
                
                VerticalScrollingFixView {
                    ScrollViewReader { proxy in
                        // Native indicator off: the hover scrollbar below is
                        // consistent with the VU/viewer strips.
                        ScrollView(.horizontal, showsIndicators: false) {
                            LazyHStack(spacing: Self.candidateSpacing) {
                                ForEach(Array(candidates.enumerated()), id: \.offset) { index, file in
                                    candidateMemberView(file: file, index: index)
                                }
                            }
                            .padding(.top, 30)
                            .padding(.bottom, 30)
                            .padding(.leading, Self.candidateLeadingPadding)
                            .padding(.trailing, 12)
                            .overlay(alignment: .bottomLeading) {
                                let indices = candidates.enumerated().compactMap { index, file in
                                    DuplicateEligibility.distance(similarityScore: file.similarityScore) <= duplicateTightness ? index : nil
                                }
                                let widths = candidateWidths
                                let prefix = candidatePrefixOffsets
                                
                                if let first = indices.first, let last = indices.last {
                                    // Prefix-sum over the actual tile widths (§6.5). The
                                    // -4 / +8 keep the bracket's 4 pt overhang on each side.
                                    let leftOffset = prefix[first] + Self.candidateLeadingPadding - 4
                                    let width = (prefix[last] + widths[last]) - prefix[first] + 8
                                    
                                    let toRestore = indices.compactMap { candidates[$0] }.filter { $0.isEjected }.map { $0.filePath }
                                    let toEject = indices.compactMap { candidates[$0] }.filter { !$0.isEjected }.map { $0.filePath }
                                    let isRestoreMode = toEject.isEmpty
                                    
                                    BracketView(
                                        width: width,
                                        isRestoreMode: isRestoreMode,
                                        isEjecting: isEjecting,
                                        action: {
                                            if isRestoreMode {
                                                onRestoreSet(toRestore)
                                            } else if !toEject.isEmpty {
                                                onEjectSet(toEject)
                                            }
                                        }
                                    )
                                    .padding(.leading, leftOffset)
                                    .padding(.bottom, 6)
                                    .animation(.spring(response: 0.3, dampingFraction: 0.7), value: width)
                                    .animation(.spring(response: 0.3, dampingFraction: 0.7), value: leftOffset)
                                }
                            }
                        }
                        .hoverHorizontalScrollbar(totalUnits: candidates.count,
                                                  loadedUnits: candidates.count)
                        .onChange(of: selectedCandidateIndex) { old, newIdx in
                            if isSelected {
                                let safeIdx = min(max(0, newIdx), max(0, candidates.count - 1))
                                if safeIdx != newIdx {
                                    DispatchQueue.main.async {
                                        selectedCandidateIndex = safeIdx
                                    }
                                } else {
                                    withAnimation(.easeOut(duration: 0.15)) {
                                        proxy.scrollTo(safeIdx, anchor: .center)
                                    }
                                }
                            }
                        }
                        .onChange(of: isSelected) { old, newlySelected in
                            if newlySelected {
                                let safeIdx = min(max(0, selectedCandidateIndex), max(0, candidates.count - 1))
                                DispatchQueue.main.async {
                                    withAnimation(.easeOut(duration: 0.15)) {
                                        proxy.scrollTo(safeIdx, anchor: .center)
                                    }
                                }
                            }
                        }
                    }
                }
                // STRIP-1: the candidate band grows with the tile height;
                // paddings (and the hover scrollbar inside them) do not.
                .frame(height: 130 + strip.extraHeight(for: .binned))
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(isSelected ? Theme.surfaceElevated.opacity(0.8) : (isCardHovered ? Color.primary.opacity(0.05) : Theme.surface.opacity(0.5)))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(isRepEjectHovered ? Theme.error : (isSelected ? Theme.textAccent : Theme.border), lineWidth: (isRepEjectHovered || isSelected) ? 2 : 1)
        )
        // The advanced-overlay path bar is drawn as a bottom overlay inside the
        // card's existing padding (in the empty space below the thumbnails), so
        // enabling the overlay never changes the row height or the spacing around
        // the image.
        .overlay(alignment: .bottomLeading) {
            if advancedOverlayEnabled {
                advancedPathBar
                    .padding(.horizontal, 20)
                    .padding(.bottom, 6)
            }
        }
        // STRIP-1: drag either border to resize every strip; double-click resets.
        .stripResizeGrips(panel: .binned, rowIndex: rowIndex)
        .onHover { hovering in
            isCardHovered = hovering
            // Clear the displayed path once the pointer leaves the whole group.
            if !hovering { lastHoveredPath = nil }
        }
        .padding(.bottom, 4)
    }

    // Path of the last-hovered thumbnail with a copy button. Rendered as a fixed
    // 16pt-high bar; placed by the caller as a bottom overlay so it occupies the
    // empty space below the thumbnails without adding layout height.
    @ViewBuilder
    private var advancedPathBar: some View {
        HStack(spacing: 6) {
            if let path = lastHoveredPath {
                Text(path)
                    .font(.custom("Menlo", size: 9))
                    .foregroundColor(Theme.success)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(path)
                Button(action: {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(path, forType: .string)
                }) {
                    Image(systemName: "doc.on.doc")
                        .font(.system(size: 9))
                        .foregroundColor(Theme.success)
                }
                .buttonStyle(.plain)
                .help("Copy path")
            }
        }
        .frame(maxWidth: .infinity, minHeight: 16, maxHeight: 16, alignment: .leading)
    }

    // The representative thumbnail. Extracted from `body` to keep that expression
    // small enough for the Swift type-checker.
    @ViewBuilder
    private func representativeMemberView(_ rep: Vurio_Dedup_DuplicateFile) -> some View {
        DuplicateMemberView(file: rep, isRep: true, overlayText: "ID: \(group.groupID)", isEjecting: isEjecting, onRestore: {
            onRestoreSet(group.files.map { $0.filePath })
        }, onEject: {
            onEjectSet(group.files.map { $0.filePath })
        }, onEjectHoverChanged: { hovering in
            withAnimation(.easeInOut(duration: 0.2)) {
                isRepEjectHovered = hovering
            }
        }, onThumbnailTapped: {
            if !isEjecting {
                if let idx = group.files.firstIndex(where: { $0.filePath == rep.filePath }) {
                    onThumbnailTapped(idx)
                }
            }
        }, onThumbnailDoubleTapped: {
            if !isEjecting {
                if let idx = group.files.firstIndex(where: { $0.filePath == rep.filePath }) {
                    onThumbnailDoubleTapped(idx)
                }
            }
        }, onLocate: {
            onLocateSet(rep.filePath)
        }, onThumbnailHoverChanged: { hovering in
            if hovering { lastHoveredPath = rep.filePath }
        })
        .padding(.top, 25)
        .padding(.bottom, 25)
    }

    // A single candidate thumbnail. Extracted from `body` for the same reason.
    @ViewBuilder
    private func candidateMemberView(file: Vurio_Dedup_DuplicateFile, index: Int) -> some View {
        DuplicateMemberView(file: file, isRep: false, isEjecting: isEjecting, onRestore: {
            onRestoreSet([file.filePath])
        }, onEject: {
            onEjectSet([file.filePath])
        }, onThumbnailTapped: {
            if !isEjecting {
                // Single-click focuses the candidate (accent border + keyboard
                // anchor) and selects the row via the callback.
                selectedCandidateIndex = index
                if let idx = group.files.firstIndex(where: { $0.filePath == file.filePath }) {
                    onThumbnailTapped(idx)
                }
            }
        }, onThumbnailDoubleTapped: {
            if !isEjecting {
                selectedCandidateIndex = index
                if let idx = group.files.firstIndex(where: { $0.filePath == file.filePath }) {
                    onThumbnailDoubleTapped(idx)
                }
            }
        }, onLocate: {
            onLocateSet(file.filePath)
        }, onThumbnailHoverChanged: { hovering in
            if hovering { lastHoveredPath = file.filePath }
        }, isFocusedCandidate: isSelected && selectedCandidateIndex == index)
        .id(index)
    }
}

struct DuplicateMemberView: View {
    let file: Vurio_Dedup_DuplicateFile
    let isRep: Bool
    var overlayText: String? = nil
    var isEjecting: Bool = false
    var onRestore: (() -> Void)? = nil
    var onEject: (() -> Void)? = nil
    var onEjectHoverChanged: ((Bool) -> Void)? = nil
    var onThumbnailTapped: (() -> Void)? = nil
    // Double-click opens the VIEWER tab. Single-click (onThumbnailTapped) only
    // selects, so it also fires on the first click of a double-click — that is
    // deliberate: selection is a prerequisite of viewing, and it keeps
    // single-click feedback instant (no double-click disambiguation delay).
    var onThumbnailDoubleTapped: (() -> Void)? = nil
    var onLocate: (() -> Void)? = nil
    var onThumbnailHoverChanged: ((Bool) -> Void)? = nil
    var isFocusedCandidate: Bool = false
    // When false, this tile exposes no eject/restore affordance and never shows
    // the ejected (red) border. Used by the VU day view, which is read-only for
    // eject — all eject management lives on the BINNED tab.
    var allowEject: Bool = true
    // Overrides the isRep-based tile HEIGHT (80/70). The VU day view pins every
    // tile to 70. (THUMB-1: the tile's width is derived from the file's display
    // geometry, and every tile shares one ServerImageLoader.tileAssetSize asset
    // regardless of its display size, so this no longer affects cache sharing.)
    var fixedSize: CGFloat? = nil
    // When false, the advanced overlay omits the per-tile match (similarity)
    // score. The VU day view hides scores; BINNED keeps them.
    var showMatchScore: Bool = true
    // Which strip this tile is drawn in, so it follows that panel's own
    // height. VU pins its tiles with `fixedSize`, so this only matters for
    // which panel's changes it redraws for.
    var panel: StripLayout.Panel = .binned

    @State private var isHovered = false
    @State private var isButtonHovered = false
    @State private var isMaskHovered = false
    // Timestamp of this tile's own last single-tap delivery, for identity-
    // anchored double-click detection. See the tap gesture below.
    @State private var lastTapTime: Date? = nil
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false

    /// The file's source volume is disconnected and it is not in the ejected bin —
    /// i.e. genuinely missing. The thumbnail is masked and the only allowed action
    /// is to locate the file at its new on-disk location.
    private var isMissing: Bool {
        !file.isAvailable && !file.isEjected
    }

    @ObservedObject private var strip = StripLayout.shared

    /// THUMB-1: fixed tile height; the width follows the image's display
    /// geometry (clamped 1:3…3:1, never cropped) — see ThumbnailGeometry.
    /// STRIP-1: `stripTileHeight` is the owning panel's dragged height (70 by default);
    /// the representative keeps its +10 pt prominence over candidates.
    static func tileHeight(isRep: Bool, fixedSize: CGFloat?, stripTileHeight: CGFloat) -> CGFloat {
        fixedSize ?? (isRep ? stripTileHeight + 10 : stripTileHeight)
    }

    static func tileSize(for file: Vurio_Dedup_DuplicateFile, isRep: Bool, fixedSize: CGFloat?, stripTileHeight: CGFloat) -> CGSize {
        ThumbnailGeometry.tileSize(height: tileHeight(isRep: isRep, fixedSize: fixedSize, stripTileHeight: stripTileHeight),
                                   displayWidth: file.displayWidth,
                                   displayHeight: file.displayHeight)
    }

    var body: some View {
        let tile = Self.tileSize(for: file, isRep: isRep, fixedSize: fixedSize, stripTileHeight: strip.tileHeight(for: panel))
        let width = tile.width
        let height = tile.height
        let cornerRadius: CGFloat = 8

        ZStack {
            // One shared asset (tileAssetSize) for every surface — the
            // representative displays at 80 pt tall, candidates and VU tiles at
            // 70, all off the same cache entry.
            ThumbnailView(filePath: file.filePath, size: tile, cachedThumbnailPath: file.cachedThumbnailPath, cachedThumbnails: file.cachedThumbnails, requestSize: ServerImageLoader.tileAssetSize, renderRevision: Int(file.renderRevision))
                .frame(width: width, height: height)
                .cornerRadius(cornerRadius)
                .opacity(file.isEjected ? 0.3 : 1.0)
                .contentShape(Rectangle())
                .onTapGesture {
                    guard !isMissing else { return }
                    onThumbnailTapped?()
                    // A native TapGesture(count: 2) recognizes off the raw
                    // NSEvent.clickCount at the click's SCREEN position — tracked
                    // by AppKit independent of which view ends up hit-testing
                    // each click. If this tile's photo shifts under the cursor
                    // between the two clicks (day/candidate strips reflow live
                    // while files are still being imported/processed), the
                    // second click's "double-click" fires on whatever DIFFERENT
                    // tile now sits there instead. Comparing timestamps across
                    // repeated single-tap delivery to THIS tile's own identity
                    // means a double-click only fires when the same logical
                    // file was tapped twice in a row — worst case on a reflow
                    // is a missed double-click, never opening the wrong image.
                    let now = Date()
                    if let last = lastTapTime, now.timeIntervalSince(last) <= NSEvent.doubleClickInterval {
                        lastTapTime = nil
                        onThumbnailDoubleTapped?()
                    } else {
                        lastTapTime = now
                    }
                }

            if isMissing {
                // Etched mask: covers the whole thumbnail, swallows every other
                // interaction, and exposes a single "locate" affordance.
                Button(action: { onLocate?() }) {
                    ZStack {
                        RoundedRectangle(cornerRadius: cornerRadius)
                            .fill(Color.black.opacity(isMaskHovered ? 0.7 : 0.55))
                        VStack(spacing: 3) {
                            Image(systemName: "questionmark.folder.fill")
                                .font(.system(size: isRep ? 20 : 17, weight: .semibold))
                            Text("Locate")
                                .font(.system(size: 9, weight: .semibold))
                        }
                        .foregroundColor(Theme.textPrimary)
                    }
                    .frame(width: width, height: height)
                }
                .buttonStyle(.plain)
                .help("This file's drive is disconnected. Click to locate it in its new location.")
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isMaskHovered = hovering
                    }
                }
            }

            if file.isEjected && allowEject {
                Button(action: {
                    onRestore?()
                }) {
                    CustomRestoreShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 16, height: 16)
                        .frame(width: 40, height: 40)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isButtonHovered && !isEjecting ? Theme.success : Theme.success.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .disabled(isEjecting)
                .opacity(isEjecting ? 0.5 : 1.0)
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isButtonHovered = hovering
                    }
                }
            }
        }
        .overlay(
            Group {
                if isMissing {
                    RoundedRectangle(cornerRadius: cornerRadius)
                        .stroke(Theme.border, style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
                } else {
                    RoundedRectangle(cornerRadius: cornerRadius)
                        .stroke((file.isEjected && allowEject) ? Theme.error : (isFocusedCandidate ? Theme.textAccent : (isRep ? Theme.textAccent : (isHovered ? Theme.textPrimary : Theme.border))), lineWidth: isFocusedCandidate ? 3 : (isRep ? 2 : ((file.isEjected && allowEject) ? 2 : 1)))
                }
            }
        )
        .overlay(alignment: .topLeading) {
            if advancedOverlayEnabled {
                if let text = overlayText {
                    Text(text)
                        .font(.custom("Menlo", size: 9))
                        .foregroundColor(Theme.success)
                        .offset(y: -14)
                } else if !isRep && showMatchScore {
                    Text(file.similarityScore.formatted(.number.precision(.fractionLength(4))))
                        .font(.custom("Menlo", size: 9))
                        .foregroundColor(Theme.success)
                        .offset(y: -14)
                }
            }
        }
        .overlay(alignment: .topTrailing) {
            if !file.isEjected && !isEjecting && !isMissing && allowEject {
                Button(action: {
                    onEject?()
                }) {
                    CustomEjectShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 10, height: 10)
                        .frame(width: 20, height: 20)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isButtonHovered ? Theme.error : Theme.error.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .offset(x: 8, y: -8)
                .opacity((isHovered || isButtonHovered) ? 1.0 : 0.0)
                .allowsHitTesting(isHovered || isButtonHovered)
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isButtonHovered = hovering
                    }
                    onEjectHoverChanged?(hovering)
                }
            }
        }
        .scaleEffect(isHovered && !file.isEjected && !isMissing ? 1.05 : 1.0)
        .background(
            RoundedRectangle(cornerRadius: cornerRadius)
                .fill(Color.black.opacity(0.01))
                .shadow(color: isHovered ? Color.black.opacity(0.3) : Color.clear, radius: 4, x: 0, y: 2)
        )
        .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isHovered)
        .onHover { hovering in
            isHovered = hovering
            onThumbnailHoverChanged?(hovering)
        }
        .zIndex(isHovered ? 1 : 0)
    }
}

struct BracketShape: Shape {
    var cornerRadius: CGFloat = 8
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let r = min(cornerRadius, rect.height)
        
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY - r))
        path.addArc(tangent1End: CGPoint(x: rect.minX, y: rect.maxY), tangent2End: CGPoint(x: rect.minX + r, y: rect.maxY), radius: r)
        path.addLine(to: CGPoint(x: rect.maxX - r, y: rect.maxY))
        path.addArc(tangent1End: CGPoint(x: rect.maxX, y: rect.maxY), tangent2End: CGPoint(x: rect.maxX, y: rect.maxY - r), radius: r)
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        
        return path
    }
}

struct BracketView: View {
    var width: CGFloat
    var isRestoreMode: Bool
    var isEjecting: Bool = false
    var action: () -> Void
    @State private var isHovering = false

    var body: some View {
        ZStack(alignment: .top) {
            BracketShape()
                .stroke(isRestoreMode ? Theme.success : Theme.error, style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                .frame(width: width, height: 8)
            
            Button(action: action) {
                if isRestoreMode {
                    CustomRestoreShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isHovering && !isEjecting ? Theme.success : Theme.success.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                } else {
                    CustomEjectShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isHovering && !isEjecting ? Theme.error : Theme.error.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
            }
            .buttonStyle(.plain)
            .disabled(isEjecting)
            .opacity(isEjecting ? 0.5 : 1.0)
            .offset(y: -4)
            .scaleEffect(isHovering && !isEjecting ? 1.1 : 1.0)
            .onHover { hovering in
                withAnimation(.easeInOut(duration: 0.1)) {
                    isHovering = hovering
                }
            }
        }
        .frame(width: width, height: 20)
    }
}

struct CustomEjectShape: Shape {
    func path(in rect: CGRect) -> Path {
        let scale = min(rect.width, rect.height) / 24.0
        var path = Path()
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 12 * scale))
        
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 9 * scale, y: 5 * scale))
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 15 * scale, y: 5 * scale))
        
        path.move(to: CGPoint(x: 4 * scale, y: 14 * scale))
        path.addLine(to: CGPoint(x: 4 * scale, y: 18 * scale))
        path.addQuadCurve(to: CGPoint(x: 6 * scale, y: 20 * scale), control: CGPoint(x: 4 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 18 * scale, y: 20 * scale))
        path.addQuadCurve(to: CGPoint(x: 20 * scale, y: 18 * scale), control: CGPoint(x: 20 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 20 * scale, y: 14 * scale))
        return path
    }
}

struct CustomRestoreShape: Shape {
    func path(in rect: CGRect) -> Path {
        let scale = min(rect.width, rect.height) / 24.0
        var path = Path()
        path.move(to: CGPoint(x: 12 * scale, y: 3 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        
        path.move(to: CGPoint(x: 8 * scale, y: 11 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        path.move(to: CGPoint(x: 16 * scale, y: 11 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        
        path.move(to: CGPoint(x: 4 * scale, y: 14 * scale))
        path.addLine(to: CGPoint(x: 4 * scale, y: 18 * scale))
        path.addQuadCurve(to: CGPoint(x: 6 * scale, y: 20 * scale), control: CGPoint(x: 4 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 18 * scale, y: 20 * scale))
        path.addQuadCurve(to: CGPoint(x: 20 * scale, y: 18 * scale), control: CGPoint(x: 20 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 20 * scale, y: 14 * scale))
        return path
    }
}
