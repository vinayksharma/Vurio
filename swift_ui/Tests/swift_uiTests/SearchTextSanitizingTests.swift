import Testing
@testable import VURIO

// A stalled main thread flushes a held key's whole repeat burst into the
// search field at once — observed as ~35 spaces dropped into the middle of
// "pictures from july this year". Runs of blanks collapse so the query stays
// the one the user meant.
@Test @MainActor func collapsesABurstOfSpacesMidQuery() {
    let burst = "pictures from" + String(repeating: " ", count: 35) + "july this year"
    #expect(AllDaysViewModel.collapsingBlankRuns(burst) == "pictures from july this year")
}

// Typing a word boundary must survive untouched — a trailing space is
// mid-typing, not a run.
@Test @MainActor func leavesOrdinaryTypingAlone() {
    #expect(AllDaysViewModel.collapsingBlankRuns("dog on the beach") == "dog on the beach")
    #expect(AllDaysViewModel.collapsingBlankRuns("dog on the ") == "dog on the ")
    #expect(AllDaysViewModel.collapsingBlankRuns("") == "")
    #expect(AllDaysViewModel.collapsingBlankRuns(" ") == " ")
}

@Test @MainActor func collapsesTabsAndNewlinesToASingleSpace() {
    #expect(AllDaysViewModel.collapsingBlankRuns("july\tthis\nyear") == "july this year")
    #expect(AllDaysViewModel.collapsingBlankRuns("july \t \n this") == "july this")
}

// Leading blanks collapse to one rather than vanishing: the debounced query
// is trimmed before it goes out, so the field can keep what was typed.
@Test @MainActor func collapsesLeadingRunWithoutTrimming() {
    #expect(AllDaysViewModel.collapsingBlankRuns("   july") == " july")
}
