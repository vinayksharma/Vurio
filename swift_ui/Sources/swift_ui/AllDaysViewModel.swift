import SwiftUI
import GRPC

struct DaySlot: Identifiable {
    let id = UUID()
    var day: Vurio_Dedup_DayGroup?
    var placeholderIndex: Int = 0
}

/// Paging view model for the day-based "ALL" view. Mirrors DuplicatesViewModel's
/// slot/placeholder paging, but pages over capture days (GetDayGroups) and learns
/// the total from each response instead of from AppState. Bulk eject/restore and
/// the slider stay on DuplicatesViewModel — this model only reads.
@MainActor
class AllDaysViewModel: ObservableObject {
    @Published var days: [DaySlot] = []
    @Published var totalDays: Int32 = 0
    @Published var loadingTasks: [Int32: Task<Void, Never>] = [:]

    private var prefetchTasks: [Int: Task<Void, Never>] = [:]

    // Days are heavier than duplicate groups (a day can hold hundreds of files),
    // so page fewer of them at a time.
    let pageSize: Int32 = 10

    /// Reset the slot list and load the first page to learn the day count.
    func refresh() async {
        for task in loadingTasks.values { task.cancel() }
        loadingTasks.removeAll()
        for task in prefetchTasks.values { task.cancel() }
        prefetchTasks.removeAll()
        days = []
        await loadPage(pageIndex: 0)
    }

    func updateVisibleRows(_ range: ClosedRange<Int>) {
        let prefetchMargin = 5
        let start = max(0, range.lowerBound - prefetchMargin)
        let end = min(max(0, days.count - 1), range.upperBound + prefetchMargin)
        guard start <= end else { return }

        let startPage = Int32(start / Int(pageSize))
        let endPage = Int32(end / Int(pageSize))

        for p in loadingTasks.keys {
            if p < startPage || p > endPage {
                loadingTasks[p]?.cancel()
                loadingTasks[p] = nil
            }
        }

        for p in startPage...endPage {
            triggerLoadPage(p)
        }

        let desiredPrefetch = Set(start...end).subtracting(Set(range))

        for (index, task) in prefetchTasks {
            if index < start || index > end {
                task.cancel()
                prefetchTasks.removeValue(forKey: index)
            }
        }

        for index in desiredPrefetch {
            if prefetchTasks[index] == nil {
                guard index < days.count, let day = days[index].day else { continue }

                let task = Task.detached(priority: .background) {
                    for file in day.files {
                        try? Task.checkCancellation()
                        if ImageCache.shared.getImage(forKey: "\(file.filePath)_70") == nil {
                            _ = try? await ServerImageLoader.shared.loadThumbnail(filePath: file.filePath, size: 70, cachedThumbnailPath: file.cachedThumbnailPath)
                        }
                    }
                }
                prefetchTasks[index] = task
            }
        }
    }

    func triggerLoadPage(_ pageIndex: Int32) {
        guard loadingTasks[pageIndex] == nil else { return }

        let task = Task {
            await loadPage(pageIndex: pageIndex)
        }
        loadingTasks[pageIndex] = task
    }

    /// Re-fetch only the pages that already have data so server-computed fields
    /// (isEjected/isAvailable, group membership) update in place — after bulk
    /// eject/restore or a volume mount/unmount — without resetting to placeholders.
    func reloadLoadedPages() async {
        let loadedPages: [Int32] = Set(
            days.enumerated()
                .filter { $0.element.day != nil }
                .map { Int32($0.offset) / pageSize }
        ).sorted()
        for page in loadedPages {
            loadingTasks[page]?.cancel()
            loadingTasks[page] = nil
            await loadPage(pageIndex: page)
        }
    }

    func loadPage(pageIndex: Int32) async {
        do {
            guard let client = VurioGrpcClient.shared.duplicates else {
                loadingTasks[pageIndex] = nil
                return
            }

            let request = Vurio_Dedup_GetDayGroupsRequest.with {
                $0.limit = pageSize
                $0.offset = pageIndex * pageSize
            }

            let response = try await client.getDayGroups(request)

            if !Task.isCancelled {
                // Resize the slot list when the server's day count moved (files
                // scanned/removed); existing slots keep their stable ids.
                if response.totalDays != totalDays || days.count != Int(response.totalDays) {
                    totalDays = response.totalDays
                    var resized = days
                    if resized.count > Int(totalDays) {
                        resized.removeLast(resized.count - Int(totalDays))
                    } else {
                        for i in resized.count..<Int(totalDays) {
                            resized.append(DaySlot(day: nil, placeholderIndex: i))
                        }
                    }
                    days = resized
                }

                var localDays = days
                for (i, day) in response.days.enumerated() {
                    let globalIndex = Int(pageIndex * pageSize) + i
                    if globalIndex < localDays.count {
                        localDays[globalIndex].day = day
                    }
                }
                days = localDays
            }
            loadingTasks[pageIndex] = nil

        } catch {
            let isCancelled: Bool
            if error is CancellationError {
                isCancelled = true
            } else if let grpcStatus = error as? GRPCStatus, grpcStatus.code == .cancelled {
                isCancelled = true
            } else {
                isCancelled = false
            }

            if !isCancelled {
                MessageCenter.shared.post("Failed to load days page \(pageIndex): \(error.localizedDescription)")
            }
            loadingTasks[pageIndex] = nil
        }
    }

    /// Optimistically flip isEjected on loaded rows so thumbnails update
    /// immediately after a per-file or bulk eject/restore; authoritative state is
    /// reconciled via reloadLoadedPages().
    func applyEjectionState(_ paths: [String], isEjected: Bool) {
        let pathSet = Set(paths)
        for di in days.indices {
            guard var day = days[di].day else { continue }
            var changed = false
            for fi in day.files.indices where pathSet.contains(day.files[fi].filePath) {
                day.files[fi].isEjected = isEjected
                changed = true
            }
            if changed {
                days[di].day = day
            }
        }
    }
}
