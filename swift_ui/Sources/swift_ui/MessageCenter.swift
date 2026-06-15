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

    /// How long a message stays before auto-dismissing.
    static let defaultDuration: TimeInterval = 7

    /// The message currently shown, or `nil` when nothing is displayed.
    @Published private(set) var message: String?

    private var dismissTask: Task<Void, Never>?

    private init() {}

    /// Post a transient status message. It replaces any message currently shown
    /// and auto-dismisses after `duration` seconds.
    /// Safe to call from any `@MainActor` context (all view-models here are).
    func post(_ text: String, duration: TimeInterval = MessageCenter.defaultDuration) {
        message = text
        scheduleDismiss(after: duration)
    }

    /// Keep the current message visible for another full duration (e.g. when the
    /// user clicks it). No-op if nothing is currently shown.
    func keepAlive(for duration: TimeInterval = MessageCenter.defaultDuration) {
        guard message != nil else { return }
        scheduleDismiss(after: duration)
    }

    /// Immediately dismiss the current message.
    func dismiss() {
        dismissTask?.cancel()
        dismissTask = nil
        message = nil
    }

    private func scheduleDismiss(after duration: TimeInterval) {
        dismissTask?.cancel()
        dismissTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(duration * 1_000_000_000))
            guard !Task.isCancelled else { return }
            self?.message = nil
        }
    }
}
