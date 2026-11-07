import Foundation
import Testing
@testable import PhotonStackUI

@Test func reviewChangingAFilteredDecisionLeavesOtherFramesUnmarked() {
    var focus = DeepSkyReviewFocus()
    let listed = ["a.fit", "b.fit", "c.fit"]
    focus.open("a.fit")
    focus.select(["a.fit", "b.fit"], in: listed)
    #expect(focus.focused == "b.fit")
    focus.reconcile(with: ["c.fit"])
    #expect(focus.selected.isEmpty && focus.focused == nil)
    // Undo puts the old paths back in the list, without granting edit targets.
    focus.reconcile(with: listed)
    #expect(focus.selected.isEmpty && focus.focused == nil)
    focus.open("c.fit")
    #expect(focus.selected == ["c.fit"] && focus.focused == "c.fit")
}

@Test func reviewChangingAListKeepsOnlySurvivingSelectedPaths() {
    var focus = DeepSkyReviewFocus()
    let listed = ["a.fit", "b.fit", "c.fit"]
    focus.open("a.fit")
    focus.select(Set(listed), in: listed)
    focus.reconcile(with: ["b.fit", "c.fit", "other.fit"])
    #expect(focus.selected == ["b.fit", "c.fit"] && focus.focused == "b.fit")
    focus.reconcile(with: ["same-ordinal-new-source.fit"])
    #expect(focus.selected.isEmpty && focus.focused == nil)
}

@Test @MainActor func reviewNativeTableReplacesAllRowsWithoutChoosingAnAdjacentExposure() {
    let coordinator = DeepSkyReviewTable.Coordinator()
    let all = (0..<12).map { "frame-\($0)" }
    let excluded = [all[2], all[5], all[8]]
    var edits: [Set<String>] = []
    func update(_ paths: [String], selected: Set<String>) {
        coordinator.update(paths: paths, selected: selected, focused: selected.sorted().first, redraw: true,
            row: { .init(index: $0, filename: paths[$0], decision: "decision", symbol: "photo", kept: true) },
            select: { edits.append($0) })
    }
    update(all, selected: [all[10]])
    update(excluded, selected: [excluded[2]])
    #expect(coordinator.table.numberOfRows == 3 && coordinator.table.selectedRowIndexes == IndexSet(integer: 2))
    update([], selected: [excluded[2]])
    #expect(coordinator.table.numberOfRows == 0 && coordinator.table.selectedRowIndexes.isEmpty)
    update(excluded, selected: [])
    update(all, selected: [])
    #expect(coordinator.table.numberOfRows == 12 && coordinator.table.selectedRowIndexes.isEmpty && edits.isEmpty)
    // Native keyboard and modifier-click selection goes through the same delegate.
    coordinator.table.selectRowIndexes(IndexSet([2, 8]), byExtendingSelection: false)
    #expect(edits.last == [all[2], all[8]])
}
