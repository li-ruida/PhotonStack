import Testing
import Foundation
@testable import PhotonStackUI

@Test func packagedBuildIdentityIsCanonicalForDisplayAndCache() {
    let identity = PhotonStackBuildIdentity(
        version: "1.2.3",
        build: "20260722010101",
        commit: "abc123",
        buildDate: "2026-07-22T01:01:01Z",
        bundleIdentity: "  v1.2.3 (abc123, 20260722010101)  ",
        executableFingerprint: "dev-old"
    )

    #expect(identity.displayText == "v1.2.3 (abc123, 20260722010101)")
    #expect(identity.cacheIdentity == "v1.2.3 (abc123, 20260722010101)")
    #expect(identity.helpText.contains("Executable:") == false)
}

@Test func developmentBuildIdentityChangesWithExecutableFingerprint() {
    let first = PhotonStackBuildIdentity(
        version: "dev",
        build: "local",
        commit: "local",
        buildDate: "local",
        bundleIdentity: "   ",
        executableFingerprint: "dev-first"
    )
    let second = PhotonStackBuildIdentity(
        version: "dev",
        build: "local",
        commit: "local",
        buildDate: "local",
        bundleIdentity: nil,
        executableFingerprint: "dev-second"
    )

    #expect(first.cacheIdentity != second.cacheIdentity)
    #expect(first.displayText == "development (dev-first)")
    #expect(first.helpText.contains("Executable: dev-first"))
}

@Test func developmentExecutableFingerprintTracksBinaryAndProcessIdentity() throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackBuildIdentity-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("PhotonStackMac")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try Data("first".utf8).write(to: executable)
    let first = PhotonStackBuildIdentity.executableFingerprint(at: executable)

    try Data("second-build-with-a-different-size".utf8).write(to: executable)
    let second = PhotonStackBuildIdentity.executableFingerprint(at: executable)

    #expect(first != second)
    #expect(PhotonStackBuildIdentity.executableFingerprint(at: nil, processIdentifier: 42) == "process-42")
}
