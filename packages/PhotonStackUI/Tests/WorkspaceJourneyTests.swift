import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackUI

@Test func journeyRequiresImportThenAnalysisThenReview() {
    var state = WorkspaceJourneyState(lightCount: 0, analysisCurrent: false, resultCurrent: false, finishingRequested: false)
    #expect(state.step == .importPhotos)
    #expect(!state.completed(.importPhotos))
    state.lightCount = 2
    #expect(state.step == .importPhotos)
    state.lightCount = 3
    #expect(state.step == .review)
    #expect(state.next == .review)
    state.analysisCurrent = true
    #expect(state.step == .stack)
    #expect(state.next == .review) // Opens decisions, never starts a long job from navigation.
    #expect(state.completed(.review))
    #expect(!state.completed(.stack))
}

@Test func journeyResultLeadsToFinishingButDoesNotClaimExport() {
    let state = WorkspaceJourneyState(lightCount: 12, analysisCurrent: true, resultCurrent: true, finishingRequested: false)
    #expect(state.step == .finish)
    #expect(state.next == .export)
    #expect(state.completed(.stack))
    #expect(!state.completed(.finish))
    #expect(!state.completed(.export))
}

@Test func journeyDirectEditingDoesNotClaimSkippedStacking() {
    let state = WorkspaceJourneyState(lightCount: 1, analysisCurrent: false, resultCurrent: false, finishingRequested: true)
    #expect(state.step == .finish)
    #expect(state.next == .export)
    #expect(!state.completed(.review))
    #expect(!state.completed(.stack))
}

@Test func journeyStaleAnalysisOrResultReturnsToRequiredStep() {
    var state = WorkspaceJourneyState(lightCount: 12, analysisCurrent: true, resultCurrent: false, finishingRequested: false)
    #expect(state.step == .stack)
    state.analysisCurrent = false
    #expect(state.step == .review)
    #expect(!state.completed(.review))
    state.finishingRequested = true // Still permit inspecting/editing a previous result.
    #expect(state.step == .finish)
    #expect(!state.completed(.stack))
}

@Test func journeyOnlyConfirmsSuccessfulImageExports() {
    let output = URL(fileURLWithPath: "/tmp/journey-result.tiff")
    let failed = ProcessingJob(title: "Export Image", kind: .export, status: .failed, outputURL: output)
    let cancelled = ProcessingJob(title: "Export Image", kind: .export, status: .cancelled, outputURL: output)
    let saved = ProcessingJob(title: "Save Project", kind: .export, status: .succeeded, outputURL: output)
    let success = ProcessingJob(title: "Export Image", kind: .export, status: .succeeded, outputURL: output)
    #expect(WorkspaceJourneyState.lastImageExport(in: [failed, cancelled, saved]) == nil)
    #expect(WorkspaceJourneyState.lastImageExport(in: [success, failed, cancelled, saved]) == output)
}
