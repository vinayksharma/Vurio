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
        // FACE-3: always attached (the search row is often collapsed away).
        .onReceive(PeopleNavigator.shared.$pending) { pending in
            applyPendingPersonFilter(pending)
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
                // VU-only semantic search: filters the day view to photos
                // matching the typed description (server-side CLIP search).
                if showAllMode {
                    searchFilterRow
                }
                // The identical/similar tightness slider governs duplicate
                // grouping — a BINNED concern; the VU day view gets only the
                // search row.
                if !showAllMode {
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
            VStack(spacing: 10) {
                Spacer()
                if allViewModel.isSearching {
                    // The day list is reset while a query is in flight —
                    // "no results" is not knowable yet and reads as a
                    // failed search during the LLM + CLIP round-trip.
                    ProgressView()
                        .controlSize(.small)
                    Text("Searching…")
                        .foregroundColor(Theme.textSecondary)
                } else {
                    Text(allViewModel.isSearchActive
                         ? "No photos match \"\(allViewModel.activeSearchQuery)\"."
                         : "No days available.")
                        .foregroundColor(Theme.textSecondary)
                }
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
                    anchorHandle: viewModel.anchorHandle,
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

    // FACE-3: a cluster picked in the People tab lands here (the tab switch
    // happens in ContentView, which watches the same bus).
    private func applyPendingPersonFilter(_ pending: PeopleNavigator.ClusterFilter?) {
        guard let pending else { return }
        PeopleNavigator.shared.pending = nil
        allViewModel.setPersonFilter(pending)
        // The chip lives in the filters card — make sure it's visible so the
        // filter is discoverable and clearable.
        isFiltersCollapsed = false
    }

    // VU search field: debounced in AllDaysViewModel (300 ms after the last
    // keystroke the day list reloads filtered to CLIP matches).
    @ViewBuilder
    private var searchFilterRow: some View {
        HStack(spacing: 8) {
            if let person = allViewModel.personFilter {
                // FACE-3 drill-down chip: the day list is showing one
                // person's photos (composes with search by intersection).
                HStack(spacing: 4) {
                    Image(systemName: "person.crop.circle")
                    Text(person.name.uppercased())
                        .font(.caption.weight(.bold))
                    Button(action: { allViewModel.clearPersonFilter() }) {
                        Image(systemName: "xmark.circle.fill")
                    }
                    .buttonStyle(PlainButtonStyle())
                }
                .foregroundColor(Color.black.opacity(0.85))
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(Capsule().fill(Theme.textAccent.opacity(0.9)))
            }
            // In-flight feedback: the query round-trip includes the server's
            // LLM parse (~1.5 s) — silence here reads as broken. Both states
            // are always present, cross-faded in a fixed-size slot: an
            // if/else here swapped one view for another of a different width
            // on every search, which re-laid out (and shifted the text under
            // the caret of) the field being typed into.
            ZStack {
                ProgressView()
                    .controlSize(.small)
                    .opacity(allViewModel.isSearching ? 1 : 0)
                Image(systemName: "magnifyingglass")
                    .foregroundColor(Theme.textSecondary)
                    .opacity(allViewModel.isSearching ? 0 : 1)
            }
            .frame(width: 16, height: 16)
            TextField(
                "Search photos — e.g. \"dog on the beach\"",
                text: Binding(
                    get: { allViewModel.searchText },
                    set: { allViewModel.searchText = $0 }
                )
            )
            .textFieldStyle(PlainTextFieldStyle())
            .foregroundColor(Theme.textPrimary)
            if allViewModel.isSearchActive {
                searchSourceToggle("ai", isOn: allViewModel.searchIncludeAi,
                                   count: allViewModel.searchMatchCountAi,
                                   help: "AI (semantic) pipeline. Alone, the query runs as a pure visual search of its subject — metadata facts are ignored. The count is how many matches this route contributed.")
                    { allViewModel.toggleSearchAi() }
                searchSourceToggle("xf", isOn: allViewModel.searchIncludeExif,
                                   count: allViewModel.searchMatchCountExif,
                                   help: "EXIF (fact) pipeline. Alone, only the query's metadata facts apply — no visual ranking. All on = the hybrid default. The count is how many matches this route contributed.")
                    { allViewModel.toggleSearchExif() }
                searchSourceToggle("fc", isOn: allViewModel.searchIncludeFace,
                                   count: allViewModel.searchMatchCountFace,
                                   help: "Face (people) pipeline — person names and face counts. Off, those facts are ignored; alone, only they apply. All on = the hybrid default. The count is how many matches this route contributed.")
                    { allViewModel.toggleSearchFace() }
            }
            // The server answered with the fallback pattern parser —
            // model-name and free-form facts may not have been applied.
            // Hidden rather than removed, so its arrival mid-query does not
            // resize the text field.
            let degraded = allViewModel.isSearchActive && allViewModel.searchParseDegraded
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(.orange)
                .opacity(degraded ? 1 : 0)
                .allowsHitTesting(degraded)
                .help("Simplified query parsing — the language model was unavailable for this search, so some filters (like camera model names) may not have been applied.")
            let hasText = !allViewModel.searchText.isEmpty
            Button(action: { allViewModel.clearSearch() }) {
                Image(systemName: "xmark.circle.fill")
                    .foregroundColor(Theme.textSecondary)
            }
            .buttonStyle(PlainButtonStyle())
            .opacity(hasText ? 1 : 0)
            .allowsHitTesting(hasText)
        }
        .padding(10)
        .background(Theme.surface.opacity(0.5))
        .cornerRadius(8)
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Theme.border, lineWidth: 1)
        )
        .padding([.horizontal, .bottom])
    }

    /// Height of the ai/xf/fc chips. The result-count disk is exactly this
    /// tall, so its curve is the capsule's own end cap.
    private static let searchChipHeight: CGFloat = 20

    // ai/xf/fc result-source toggle (VU search row): mirrors the tile badges'
    // look. Server-filtered — the toggle state rides on the day-view
    // request and the server filters its cached match list instantly.
    // `count` is how many files that route admitted; nil while no search is
    // active (or against a server that does not report it) and the chip is
    // then just the label.
    private func searchSourceToggle(_ label: String, isOn: Bool, count: Int32?, help: String,
                                    action: @escaping () -> Void) -> some View {
        let height = Self.searchChipHeight
        return Button(action: action) {
            HStack(spacing: 0) {
                Text(label)
                    .font(.custom("Menlo", size: 10).weight(.bold))
                    .foregroundColor(isOn ? Color.black.opacity(0.85) : Theme.textSecondary.opacity(0.7))
                    .padding(.leading, 8)
                    .padding(.trailing, count == nil ? 8 : 6)
                if let count {
                    // Flush against the trailing edge and as tall as the chip,
                    // so a 1–2 digit count is a disk sitting in the capsule's
                    // end cap; longer counts stretch it into a pill of the same
                    // radius rather than clipping.
                    Text("\(count)")
                        .font(.custom("Menlo", size: 9).weight(.bold))
                        .foregroundColor(.white.opacity(isOn ? 0.95 : 0.6))
                        .monospacedDigit()
                        .padding(.horizontal, 4)
                        .frame(minWidth: height, minHeight: height)
                        .background(
                            Capsule().fill(Theme.searchCountBadge.opacity(isOn ? 1 : 0.45))
                        )
                }
            }
            .frame(height: height)
            .background(
                Capsule().fill(isOn ? Theme.textAccent.opacity(0.9) : Theme.surface.opacity(0.6))
            )
            .clipShape(Capsule())
            .overlay(
                Capsule().stroke(isOn ? Color.clear : Theme.border, lineWidth: 1)
            )
        }
        .buttonStyle(PlainButtonStyle())
        .help(help)
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
                            // `index` is the day's offset (0 = newest) within the
                            // day list AS FILTERED, so the viewer must page with
                            // the same query or it lands on a different day.
                            openViewer(.timeline(anchorDayIndex: index,
                                                 file: file,
                                                 searchQuery: allViewModel.activeSearchQuery,
                                                 searchIncludeAi: allViewModel.searchIncludeAi,
                                                 searchIncludeExif: allViewModel.searchIncludeExif,
                                                 searchIncludeFace: allViewModel.searchIncludeFace,
                                                 filterClusterId: allViewModel.personFilter?.id ?? 0))
                        },
                        onLocateSet: { memberPath in
                            locateMissingFile(memberPath: memberPath)
                        },
                        onLoadMore: {
                            Task { await allViewModel.loadMoreFiles(day: dayGroup.day) }
                        },
                        showSearchBadges: allViewModel.isSearchActive
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
