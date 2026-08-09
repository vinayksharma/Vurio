import Foundation
import os

/// Unified-log logger for the media pipeline. Watch it with:
///   log stream --predicate 'subsystem == "com.vurio.app"' --level debug
let vurioLog = Logger(subsystem: "com.vurio.app", category: "media")

/// Debug logging, compiled out of release builds entirely. The @autoclosure
/// keeps even the message's string interpolation unevaluated when logging is
/// off — logDebug sits on the per-thumbnail hot path, where the old
/// implementation (a detached Task plus an open/write/close of
/// /tmp/vurio_debug.log per line) cost real CPU during scrolling.
@inline(__always) func logDebug(_ msg: @autoclosure () -> String) {
    #if DEBUG
    let text = msg()
    vurioLog.debug("\(text)")
    #endif
}
