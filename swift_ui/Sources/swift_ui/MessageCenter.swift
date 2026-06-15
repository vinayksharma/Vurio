import SwiftUI

/// App-wide transient status messages.
///
/// A shared singleton so any class — views *or* view-models — can post a message
/// without plumbing dependencies through initializers:
///
///     MessageCenter.shared.post("Successfully restored 41 files.")
///
/// The status bar observes the same instance as an `@EnvironmentObject` (injected
/// at the app root), slides the message in, and slides it back out after a few
/// seconds or when the user clicks it.
final class MessageCenter: ObservableObject {
    static let shared = MessageCenter()

    /// The message currently shown, or `nil` when nothing is displayed.
    @Published private(set) var message: String?

    private var dismissTask: Task<Void, Never>?

    private init() {}

    /// Post a transient status message. It replaces any message currently shown
    /// and auto-dismisses after `duration` seconds (or when the user clicks it).
    /// Safe to call from any `@MainActor` context (all view-models here are).
    func post(_ text: String, duration: TimeInterval = 5) {
        dismissTask?.cancel()
        message = text
        dismissTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(duration * 1_000_000_000))
            guard !Task.isCancelled else { return }
            self?.message = nil
        }
    }

    /// Immediately dismiss the current message (e.g. the user clicked it).
    func dismiss() {
        dismissTask?.cancel()
        dismissTask = nil
        message = nil
    }
}
