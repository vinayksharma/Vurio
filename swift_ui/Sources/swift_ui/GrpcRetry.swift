import Foundation
import GRPC

/// The gRPC status code for any thrown error. grpc-swift surfaces failures as
/// several types — `GRPCStatus` itself, but also transformable errors like
/// connection-pool exhaustion and channel errors — so classification must go
/// through `GRPCStatusTransformable` rather than casting to `GRPCStatus`
/// (those other types bridge to the useless generic "The operation couldn't
/// be completed" NSError text, which is how a transient pool error once
/// reached the status bar as a scary per-page failure).
func grpcStatusCode(of error: Error) -> GRPCStatus.Code? {
    (error as? GRPCStatusTransformable)?.makeGRPCStatus().code
}

/// The caller (or the transport on its behalf) abandoned the call — e.g. a
/// page load whose row scrolled out of the window. Never worth reporting.
func isGrpcCancellation(_ error: Error) -> Bool {
    error is CancellationError || grpcStatusCode(of: error) == .cancelled
}

/// Pressure/timing failures that clear on their own once the burst drains:
/// server starved (UNAVAILABLE), pool exhausted (RESOURCE_EXHAUSTED), timing
/// races (DEADLINE_EXCEEDED / ABORTED). Retry quietly; never alarm the user.
func isTransientGrpcError(_ error: Error) -> Bool {
    switch grpcStatusCode(of: error) {
    case .unavailable, .resourceExhausted, .deadlineExceeded, .aborted:
        return true
    default:
        return false
    }
}

/// Retry an RPC that failed transiently — the server momentarily starved
/// (e.g. DB session-pool contention while background processing runs), the
/// client's connection pool briefly exhausted — with capped exponential
/// backoff. Any other error propagates immediately, and the last transient
/// error is rethrown once attempts are exhausted, so callers never mistake a
/// busy server for an empty result. Cancellation propagates immediately (the
/// backoff sleep itself throws when the task is cancelled).
func callWithBusyRetry<R>(_ op: () async throws -> R) async throws -> R {
    var delay: UInt64 = 250_000_000 // 250ms
    let maxDelay: UInt64 = 2_000_000_000
    var attemptsLeft = 4
    while true {
        do {
            return try await op()
        } catch {
            guard attemptsLeft > 0, isTransientGrpcError(error), !isGrpcCancellation(error) else {
                throw error
            }
            attemptsLeft -= 1
            try await Task.sleep(nanoseconds: delay)
            delay = min(delay * 2, maxDelay)
        }
    }
}
