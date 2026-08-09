import SwiftUI
import GRPC
import AppKit

struct VurioDuplicatesView: View {
    // Per-tab row selection: VU and BINNED keep independent selections (and
    // scroll positions — both scrollers stay mounted) across tab switches.
    @State private var vuSelection: Int? = 0
    @State private var binnedSelection: Int? = 0
    @State private var selectedCandidateIndex: Int = 0
    // VU: path of the single-click-selected thumbnail (accent border on its tile).
    @State private var selectedVUFilePath: String? = nil

    // All data/reconciliation lives on the coordinator; the aliases below keep
    // the body reading naturally (`viewModel` = BINNED groups, `allViewModel`
    // = VU days). Child invalidations re-emit through the coordinator's
    // objectWillChange, so observing it alone keeps every read live.
    @StateObject private var coordinator = DuplicatesCoordinator()
    private var viewModel: DuplicatesViewModel { coordinator.binned }
    private var allViewModel: AllDaysViewModel { coordinator.days }

    @State private var showingBulkEjectConfirmation = false
    @State private var isEjectButtonHovered = false
    @State private var isRestoreButtonHovered = false
    @State private var isFiltersCollapsed = true
    // Mode is driven by the selected sidebar tab: BINNED (duplicate groups) vs
    // VU (every photo grouped by capture day). false = binned, true = ALL/day view.
    let showAllMode: Bool
    // Whether VU/BINNED is the selected sidebar tab at all (this panel stays
    // mounted behind SETTINGS/SCROLLER). Gates the embedded viewers' keyboard
    // shortcuts.
    let isFrontmost: Bool

    // Per-tab embedded viewer: a double-click sets the tab's request and the
    // viewer slides in over the grid; X/Esc clears it and slides the grid back.
    // Each tab keeps its own request so an open viewer survives tab switches.
    @State private var vuViewerRequest: ViewerRequest? = nil
    @State private var binnedViewerRequest: ViewerRequest? = nil

    private static let viewerSlide = Animation.spring(response: 0.4, dampingFraction: 0.85)

    @EnvironmentObject var appState: AppState

    // Fixed row height regardless of the advanced overlay. The overlay's path bar is
    // drawn inside this height (as a bottom overlay on the card), so turning the
    // advanced overlay on does not grow the row or change the spacing around images.
    private let rowHeight: CGFloat = 158
    // ALL-view day rows: same image strip plus the date-stamp header line.
    private let dayRowHeight: CGFloat = 170

    let pageSize: Int32 = 30
    
    let columns = [
        GridItem(.adaptive(minimum: 300, maximum: .infinity), spacing: 16)
    ]
    
    var body: some View {
        GeometryReader { geo in
            ZStack {
                gridContent
                    .offset(x: activeViewerOpen ? -geo.size.width : 0)
                    .allowsHitTesting(!activeViewerOpen)

                embeddedViewers(width: geo.size.width)
            }
            .clipped()
        }
    }

    private var activeViewerOpen: Bool {
        showAllMode ? vuViewerRequest != nil : binnedViewerRequest != nil
    }

    // Both viewers stay mounted so an open viewer's state (strip position,
    // selection) survives tab switches. A closed viewer parks offscreen to the
    // right; opening/closing slides it over the grid (animated at the mutation
    // sites), while tab switches swap the two instantly via opacity, matching
    // the lists below.
    @ViewBuilder
    private func embeddedViewers(width: CGFloat) -> some View {
        ViewerView(
            request: vuViewerRequest,
            isActive: isFrontmost && showAllMode && vuViewerRequest != nil,
            onClose: { closeViewer(inAllMode: true) }
        )
        .offset(x: vuViewerRequest != nil ? 0 : width)
        .opacity(showAllMode ? 1 : 0)
        .allowsHitTesting(showAllMode && vuViewerRequest != nil)

        ViewerView(
            request: binnedViewerRequest,
            isActive: isFrontmost && !showAllMode && binnedViewerRequest != nil,
            onClose: { closeViewer(inAllMode: false) }
        )
        .offset(x: binnedViewerRequest != nil ? 0 : width)
        .opacity(showAllMode ? 0 : 1)
        .allowsHitTesting(!showAllMode && binnedViewerRequest != nil)
    }

    private func openViewer(_ content: ViewerContent) {
        withAnimation(Self.viewerSlide) {
            if showAllMode {
                vuViewerRequest = ViewerRequest(content: content)
            } else {
                binnedViewerRequest = ViewerRequest(content: content)
            }
        }
    }

    private func closeViewer(inAllMode: Bool) {
        withAnimation(Self.viewerSlide) {
            if inAllMode {
                vuViewerRequest = nil
            } else {
                binnedViewerRequest = nil
            }
        }
    }

    // The grid: filters bar plus the VU/BINNED scroller stack, with all the
    // data-lifecycle modifiers. Always mounted; an embedded viewer slides in
    // over it.
    private var gridContent: some View {
        VStack(alignment: .leading, spacing: 0) {

            // Top Control Bar — collapsible "Filters" panel (collapsed by default),
            // styled after the Media Locations dropdown in Settings.
            VStack(spacing: 0) {
                Button(action: {
                    withAnimation {
                        isFiltersCollapsed.toggle()
                    }
                }) {
                    HStack(spacing: 12) {
                        Image(systemName: "line.3.horizontal.decrease.circle")
                            .font(.title3)
                            .foregroundColor(Theme.textPrimary)
                        Text("Filters")
                            .font(.title3)
                            .textCase(.uppercase)
                            .fontWeight(.semibold)
                            .foregroundColor(Theme.textPrimary)
                        Spacer()
                        Image(systemName: isFiltersCollapsed ? "chevron.right" : "chevron.down")
                            .foregroundColor(Theme.textSecondary)
                    }
                    .padding()
                    .contentShape(Rectangle())
                }
                .buttonStyle(PlainButtonStyle())

                if !isFiltersCollapsed {
                HStack {
                    HStack(spacing: 4) {
                        Image(systemName: "square.fill.on.square.fill")
                        Text("IDENTICAL")
                    }
                    .font(.caption)
                    .fontWeight(.bold)
                    .foregroundColor(Theme.textSecondary)
                    
                    // Vurio Slider
                    VurioSlider(
                        duplicateValue: Binding(
                            get: { viewModel.sliderValue },
                            set: { newValue in
                                viewModel.sliderValue = newValue
                                viewModel.duplicateTightness = newValue
                            }
                        ),
                        similarValue: Binding(
                            get: { viewModel.similarSliderValue },
                            set: { viewModel.similarSliderValue = $0 }
                        ),
                        range: 0.0...viewModel.maxDistance,
                        formatValue: { val in
                            return (1.0 - val).formatted(.number.precision(.fractionLength(3)))
                        },
                        showSimilarThumb: false
                    )
                    .frame(maxWidth: .infinity)
                    .frame(height: 42)
                    // Freeze the eligibility slider while a bulk eject/restore runs so
                    // the displayed eligible set can't shift under the in-flight op.
                    .disabled(viewModel.isProcessing)
                    .padding(.horizontal, 10)
                    
                    HStack(spacing: 4) {
                        Text("SIMILAR")
                        Image(systemName: "square.on.square")
                    }
                    .font(.caption)
                    .fontWeight(.bold)
                    .foregroundColor(Theme.textSecondary)
                    .padding(.trailing, 16)
                    
                    // Eject/Restore controls live only on the BINNED tab. The VU
                    // (ALL/day) view is read-only for eject.
                    if !showAllMode {
                        ejectRestoreControls
                    }

                }
                .padding([.horizontal, .bottom])
                }
            }
            .background(Theme.surface.opacity(0.3))
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Theme.border, lineWidth: 1)
            )
            .padding(.horizontal, 30)
            .padding(.bottom, 20)
            
            Divider().background(Theme.border)

            // List
            // (Status/error messages now slide in on the bottom status bar via
            // MessageCenter instead of rendering here.)
            if !appState.isOnline {
                VStack {
                    Spacer()
                    Text("No viewModel.groups available.")
                        .foregroundColor(Theme.textSecondary)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else {
                // Both tabs' scrollers stay mounted (opacity-toggled, like the
                // tab panels in ContentView) so switching between VU and BINNED
                // preserves each side's scroll position and selection — an
                // if/else would tear down the AppKit scroll view on every switch.
                ZStack {
                    vuList
                        .opacity(showAllMode ? 1 : 0)
                        .allowsHitTesting(showAllMode)

                    binnedList
                        .opacity(showAllMode ? 0 : 1)
                        .allowsHitTesting(!showAllMode)
                }
            }
        }
        // Cross-cutting reconciliation (viewer eject broadcasts, config saves,
        // cache clears, volume mount/unmount, location deregistration) is
        // handled inside DuplicatesCoordinator via its own subscriptions; only
        // SwiftUI-lifecycle-bound shims remain here.
        .alert("Eject Duplicates", isPresented: $showingBulkEjectConfirmation) {
            Button("Cancel", role: .cancel) { }
            Button("Eject", role: .destructive) {
                coordinator.performBulkEject()
            }
        } message: {
            Text("All images identified as duplicates will be ejected. Do you want to proceed?")
        }
        .task {
            await coordinator.initialLoad(appState: appState, isAllMode: showAllMode)
        }
        .onChange(of: showAllMode) { _, isAll in
            coordinator.modeChanged(isAll: isAll)
        }
        .onChange(of: appState.duplicateGroupsCount) { _, newCount in
            coordinator.duplicateGroupsCountChanged(to: newCount)
        }
        // Bulk eject/restore runs in the background. Integrity is guaranteed
        // server-side (each request is one atomic transaction), so there is no
        // blocking overlay: progress is shown inline on the Eject/Restore buttons
        // and the user can keep browsing. Conflicting actions are individually
        // gated by `isProcessing` (both bulk buttons, the slider, and per-card
        // eject/restore controls), so only non-mutating browsing stays enabled.
    }
    
    // Row-invalidation stamps for the two scrollers: everything the row-content
    // closures render from besides the row index and the scroller-managed
    // selection. If a new input is ever captured by renderRow/renderDayRow, it
    // must be added here or rows won't repaint when it changes.
    private var vuRowVersion: Int {
        var hasher = Hasher()
        hasher.combine(allViewModel.dataVersion)
        hasher.combine(viewModel.isProcessing)
        hasher.combine(selectedVUFilePath)
        return hasher.finalize()
    }

    private var binnedRowVersion: Int {
        var hasher = Hasher()
        hasher.combine(viewModel.dataVersion)
        hasher.combine(viewModel.duplicateTightness)
        hasher.combine(viewModel.isProcessing)
        hasher.combine(selectedCandidateIndex)
        return hasher.finalize()
    }

    // VU (ALL mode) list: one row per capture day.
    @ViewBuilder
    private var vuList: some View {
        if allViewModel.days.isEmpty {
            VStack {
                Spacer()
                Text("No days available.")
                    .foregroundColor(Theme.textSecondary)
                Spacer()
            }
            .frame(maxWidth: .infinity)
        } else {
            GeometryReader { scrollGeo in
                var customInsets = scrollGeo.safeAreaInsets
                customInsets.bottom += 30

                return VurioScroller(
                    itemCount: Int(allViewModel.totalDays),
                    rowHeight: dayRowHeight,
                    dataVersion: vuRowVersion,
                    isActive: isFrontmost && showAllMode,
                    scrollbarInsets: customInsets,
                    selection: $vuSelection,
                    onVisibleRowsChanged: { range in
                        allViewModel.updateVisibleRows(range)
                    }
                ) { index in
                    renderDayRow(at: index, rowHeight: dayRowHeight)
                }
                .background(Color.clear)
            }
        }
    }

    // BINNED list: one row per duplicate group.
    @ViewBuilder
    private var binnedList: some View {
        if viewModel.groups.isEmpty && !viewModel.isLoading {
            VStack {
                Spacer()
                Text("No viewModel.groups available.")
                    .foregroundColor(Theme.textSecondary)
                Spacer()
            }
            .frame(maxWidth: .infinity)
        } else {
            GeometryReader { scrollGeo in
                var customInsets = scrollGeo.safeAreaInsets
                customInsets.bottom += 30

                return VurioScroller(
                    itemCount: Int(viewModel.totalGroups),
                    rowHeight: rowHeight,
                    dataVersion: binnedRowVersion,
                    isActive: isFrontmost && !showAllMode,
                    scrollbarInsets: customInsets,
                    selection: $binnedSelection,
                    onVisibleRowsChanged: { range in
                        viewModel.updateVisibleRows(range)
                    }
                ) { index in
                    renderRow(at: index, rowHeight: rowHeight)
                }
                .background(Color.clear)
            }
        }
    }

    // The bulk Eject/Restore buttons for the BINNED tab. Extracted so the Filters
    // bar stays within the Swift type-checker's expression limit.
    @ViewBuilder
    private var ejectRestoreControls: some View {
        // While ejecting, the button doubles as Cancel (hover shows "Cancel");
        // clicking it cancels the eject and immediately restores the files this run
        // ejected. Enabled during its own eject so the cancel is reachable; disabled
        // while restoring.
        Button(action: {
            if viewModel.isEjecting {
                viewModel.requestCancel()
            } else {
                showingBulkEjectConfirmation = true
            }
        }) {
            ejectButtonLabel
        }
        .vurioButtonStyle(tint: Theme.error)
        .onHover { isEjectButtonHovered = $0 }
        .disabled(viewModel.isRestoring || (!viewModel.isEjecting && !viewModel.canEject))

        // While restoring, the button doubles as Cancel (hover shows "Cancel");
        // clicking stops the restore and leaves the remaining files ejected (so
        // Restore stays enabled while any remain).
        Button(action: {
            if viewModel.isRestoring {
                viewModel.requestCancel()
            } else {
                Task {
                    await viewModel.handleRestore()
                }
            }
        }) {
            restoreButtonLabel
        }
        .vurioButtonStyle(tint: Theme.success)
        .onHover { isRestoreButtonHovered = $0 }
        .disabled(viewModel.isEjecting || (!viewModel.isRestoring && !viewModel.canRestore))
    }

    // Eject button content. On hover during an eject it collapses to "Cancel";
    // otherwise it shows the eject icon / live "Ejecting" progress. Extracted to
    // keep the body expression within the Swift type-checker's limit.
    @ViewBuilder
    private var ejectButtonLabel: some View {
        HStack {
            if viewModel.isEjecting && isEjectButtonHovered {
                Text("Cancel")
            } else if viewModel.isEjecting {
                ProgressView().controlSize(.small)
                Text("Ejecting")
            } else {
                CustomEjectShape()
                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                    .frame(width: 14, height: 14)
                Text("Eject")
            }
        }
        .frame(width: (viewModel.isEjecting && !isEjectButtonHovered) ? 110 : 95)
    }

    // Restore button content. Mirrors the eject button: hover during a restore
    // collapses to "Cancel"; otherwise shows the restore icon / live progress.
    @ViewBuilder
    private var restoreButtonLabel: some View {
        HStack {
            if viewModel.isRestoring && isRestoreButtonHovered {
                Text("Cancel")
            } else if viewModel.isRestoring {
                ProgressView().controlSize(.small)
                Text("Restoring")
            } else {
                CustomRestoreShape()
                    .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                    .frame(width: 14, height: 14)
                Text("Restore")
            }
        }
        .frame(width: (viewModel.isRestoring && !isRestoreButtonHovered) ? 110 : 95)
    }

    @ViewBuilder
    private func renderRow(at index: Int, rowHeight: CGFloat) -> some View {
        if index < viewModel.groups.count {
            VurioGroupRowWrapper(
                slot: viewModel.groups[index],
                duplicateTightness: viewModel.duplicateTightness,
                isProcessing: viewModel.isProcessing,
                pageSize: viewModel.pageSize,
                isSelected: index == binnedSelection,
                selectedCandidateIndex: $selectedCandidateIndex,
                onThumbnailTapped: { _ in
                    binnedSelection = index
                },
                onThumbnailDoubleTapped: { fileIndex in
                    binnedSelection = index
                    if let group = viewModel.groups[index].group {
                        openViewer(.group(group, selectedIndex: fileIndex))
                    }
                },
                onEjectSet: { paths in
                    if let group = viewModel.groups[index].group {
                        Task { await viewModel.ejectGroupFiles(paths, for: group.groupID) }
                    }
                },
                onRestoreSet: { paths in
                    if let group = viewModel.groups[index].group {
                        Task { await viewModel.restoreGroupFiles(paths, for: group.groupID) }
                    }
                },
                onLocateSet: { memberPath in
                    locateMissingFile(memberPath: memberPath)
                },
                loadPage: { pageIndex in
                    viewModel.triggerLoadPage(pageIndex)
                }
            )
            .frame(height: rowHeight)
            .id(index)
            .contentShape(Rectangle())
            .onTapGesture {
                binnedSelection = index
            }
        }
    }

    // One ALL-view day row: a loaded DayGroupCard or a placeholder that triggers
    // its page load. Mirrors VurioGroupRowWrapper for the binned rows.
    @ViewBuilder
    private func renderDayRow(at index: Int, rowHeight: CGFloat) -> some View {
        if index < allViewModel.days.count {
            let slot = allViewModel.days[index]
            Group {
                if let dayGroup = slot.day {
                    DayGroupCard(
                        day: dayGroup,
                        isEjecting: viewModel.isProcessing,
                        isSelected: index == vuSelection,
                        selectedFilePath: selectedVUFilePath,
                        onThumbnailTapped: { file in
                            vuSelection = index
                            selectedVUFilePath = file.filePath
                        },
                        onThumbnailDoubleTapped: { file in
                            vuSelection = index
                            selectedVUFilePath = file.filePath
                            // `index` is the day's global offset (0 = newest),
                            // which anchors the viewer's library timeline.
                            openViewer(.timeline(anchorDayIndex: index, file: file))
                        },
                        onLocateSet: { memberPath in
                            locateMissingFile(memberPath: memberPath)
                        },
                        onLoadMore: {
                            Task { await allViewModel.loadMoreFiles(day: dayGroup.day) }
                        }
                    )
                    .padding(.horizontal, 30)
                } else {
                    ProgressView()
                        .frame(maxWidth: .infinity, minHeight: 150)
                        .background(Theme.surface.opacity(0.5))
                        .cornerRadius(12)
                        .padding(.horizontal, 30)
                        .onAppear {
                            let pageIndex = Int32(slot.placeholderIndex) / allViewModel.pageSize
                            allViewModel.triggerLoadPage(pageIndex)
                        }
                }
            }
            .frame(height: rowHeight)
            .id(index)
            .contentShape(Rectangle())
            .onTapGesture {
                vuSelection = index
            }
        }
    }

    /// Prompt the user for the file's new on-disk location, then ask the server to
    /// remap it. The server infers the registered media-location root from the
    /// chosen file, so the user only needs to point at the moved file itself.
    private func locateMissingFile(memberPath: String) {
        coordinator.locateMissingFile(memberPath: memberPath)
    }
}

struct VurioGroupRowWrapper: View {
    let slot: GroupSlot
    let duplicateTightness: Double
    let isProcessing: Bool
    let pageSize: Int32
    let isSelected: Bool
    @Binding var selectedCandidateIndex: Int
    let onThumbnailTapped: (Int) -> Void
    let onThumbnailDoubleTapped: (Int) -> Void
    let onEjectSet: ([String]) -> Void
    let onRestoreSet: ([String]) -> Void
    let onLocateSet: (String) -> Void
    let loadPage: (Int32) -> Void

    var body: some View {
        Group {
            if let group = slot.group {
                DuplicateGroupCard(
                    group: group,
                    duplicateTightness: duplicateTightness,
                    isEjecting: isProcessing,
                    isSelected: isSelected,
                    selectedCandidateIndex: $selectedCandidateIndex,
                    onThumbnailTapped: onThumbnailTapped,
                    onThumbnailDoubleTapped: onThumbnailDoubleTapped,
                    onEjectSet: onEjectSet,
                    onRestoreSet: onRestoreSet,
                    onLocateSet: onLocateSet
                )
                .padding(.horizontal, 30)
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, minHeight: 150)
                    .background(Theme.surface.opacity(0.5))
                    .cornerRadius(12)
                    .padding(.horizontal, 30)
                    .onAppear {
                        let pageIndex = Int32(slot.placeholderIndex) / pageSize
                        loadPage(pageIndex)
                    }
            }
        }
    }
}
