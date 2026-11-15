import Foundation
import Testing
@testable import PhotonStackUI

@Test @MainActor func releaseCheckComparesVersionsAndPromptsOnceAtLaunch() async {
    let suite = UUID().uuidString
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite) }
    var tag = "v0.10.0"
    var requests = 0
    let checker = PhotonStackUpdateChecker(version: "0.9.4", defaults: defaults) {
        requests += 1
        return tag
    }
    await checker.checkOnLaunch()
    #expect(checker.launchNotice == tag)
    checker.launchNotice = nil
    await checker.checkOnLaunch()
    #expect(requests == 1)
    #expect(checker.launchNotice == nil)
    for current in ["v0.9.4", "v0.9", "v0.8.12"] {
        tag = current
        await checker.check()
        #expect(checker.status == .current)
    }
    tag = "invalid"
    await checker.check()
    #expect(checker.status == .failed)
}

@Test @MainActor func disabledLaunchCheckPersistsAndManualCheckStillWorks() async {
    let suite = UUID().uuidString
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite) }
    let initial = PhotonStackUpdateChecker(version: "0.1.4", defaults: defaults) { nil }
    #expect(initial.automatic)
    await initial.check()
    #expect(initial.status == .noRelease)
    initial.automatic = false
    var requests = 0
    let nextLaunch = PhotonStackUpdateChecker(version: "0.1.4", defaults: defaults) {
        requests += 1
        return "v0.1.5"
    }
    await nextLaunch.checkOnLaunch()
    #expect(requests == 0)
    await nextLaunch.check()
    #expect(requests == 1)
    #expect(nextLaunch.status == .available("v0.1.5"))
    #expect(nextLaunch.launchNotice == nil)
}

@Test @MainActor func failedLaunchCheckIsQuietAndCanBeRetried() async {
    let suite = UUID().uuidString
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite) }
    var offline = true
    let checker = PhotonStackUpdateChecker(version: "0.1.4", defaults: defaults) {
        if offline { throw URLError(.notConnectedToInternet) }
        return "v0.1.4"
    }
    await checker.checkOnLaunch()
    #expect(checker.status == .failed)
    #expect(checker.launchNotice == nil)
    offline = false
    await checker.check()
    #expect(checker.status == .current)
}
