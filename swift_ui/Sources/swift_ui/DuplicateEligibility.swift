import Foundation

/// The slider eligibility rule, shared by the BINNED card bracket overlay and
/// the bulk-eject enablement check. The server implements the same rule for
/// the paths-only bulk query (GetEligibleDuplicatePaths) — keep them in sync.
enum DuplicateEligibility {
    /// Distance of a member from its group representative: 1 - score, with
    /// score <= 0 treated as an exact duplicate (distance 0).
    static func distance(similarityScore: Double) -> Double {
        1.0 - (similarityScore > 0 ? similarityScore : 1.0)
    }

    /// Would a bulk eject at `tightness` take this file?
    static func isEligible(_ file: Vurio_Dedup_DuplicateFile, tightness: Double) -> Bool {
        !file.isRepresentative && !file.isEjected
            && distance(similarityScore: file.similarityScore) <= tightness
    }
}
