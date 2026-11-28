import Foundation
import Testing
@testable import PhotonStackAppCore

@Test func auditMosaicDirectoryHintsAndOverlappingRootsAreDeduplicated() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditMosaic-\(UUID())", isDirectory: true)
    let panel = root.appendingPathComponent("panel", isDirectory: true)
    try FileManager.default.createDirectory(at: panel, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try Data().write(to: panel.appendingPathComponent("Light_test.fits"))
    let groups = try MosaicPanelFolder.discover(in: [root, URL(fileURLWithPath: panel.path, isDirectory: false), panel])
    #expect(groups.count == 1)
    #expect(groups[0].frames.count == 1)
}

@Test(arguments: [0, 1, 2]) func auditRemovingLightsPreservesReferenceAndWeightIdentity(removedIndex: Int) throws {
    var project = auditWeightedProject()
    let removed = project.assets[removedIndex].id
    project.removeAsset(removed)
    let expectedReference = ["0", "-1", "1"][removedIndex]
    let expectedWeights = ["2,3", "1,3", "1,2"][removedIndex]
    #expect(project.deepSkySettings?.values["registration.reference"] == expectedReference)
    #expect(project.deepSkySettings?.values["stack.weights"] == expectedWeights)
    let restored = try JSONDecoder().decode(PhotonStackProject.self, from: JSONEncoder().encode(project))
    #expect(restored.deepSkySettings == project.deepSkySettings)
}

@Test func auditChangingLightRolesAndAddingFramesKeepsPositionalSettingsBound() {
    var project = auditWeightedProject()
    let first = project.assets[0].id
    project.updateRole(for: first, role: .dark)
    #expect(project.deepSkySettings?.values["registration.reference"] == "0")
    #expect(project.deepSkySettings?.values["stack.weights"] == "2,3")
    project.updateRole(for: first, role: .light)
    #expect(project.deepSkySettings?.values["registration.reference"] == "1")
    #expect(project.deepSkySettings?.values["stack.weights"] == "1,2,3")
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/audit-D.fits")])
    #expect(project.deepSkySettings?.values["registration.reference"] == "1")
    #expect(project.deepSkySettings?.values["stack.weights"] == "1,2,3,1")
    let before = project.deepSkySettings
    project.addAssets(from: [project.assets[0].originalURL])
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/audit-dark.fits")], role: .dark)
    #expect(project.deepSkySettings == before)
    project.updateRole(for: project.assets[1].id, role: .flat)
    #expect(project.deepSkySettings?.values["registration.reference"] == "-1")
    #expect(project.deepSkySettings?.values["stack.weights"] == "1,3,1")
}

@Test func auditLightChangesPreserveAutomaticDefaultsAndDoNotGuessMalformedWeights() {
    var project = auditWeightedProject()
    project.deepSkySettings?.values = [:]
    project.removeAsset(project.assets[0].id)
    #expect(project.deepSkySettings?.values.isEmpty == true)
    project.deepSkySettings?.values = ["registration.reference": "-1", "stack.weights": ""]
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/audit-D.fits")])
    #expect(project.deepSkySettings?.values == ["registration.reference": "-1", "stack.weights": ""])
    project.deepSkySettings?.values["stack.weights"] = "1,2,3,4,5"
    project.removeAsset(project.assets[0].id)
    #expect(project.deepSkySettings?.values["stack.weights"] == "1,2,3,4,5")
}

private func auditWeightedProject() -> PhotonStackProject {
    var project = PhotonStackProject()
    project.addAssets(from: ["A", "B", "C"].map { URL(fileURLWithPath: "/tmp/audit-\($0).fits") })
    var settings = DeepSkySettings()
    settings.values["registration.reference"] = "1"
    settings.values["stack.weights"] = "1,2,3"
    project.deepSkySettings = settings
    return project
}

@Test func auditAssetRoleChangesKeepUnrelatedExplicitMasters() throws {
    let dark = URL(fileURLWithPath: "/tmp/audit dark \"quoted\".fits")
    let external = URL(fileURLWithPath: "/tmp/external-master.fits")
    func quote(_ value: String) -> String { "\"" + value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\"" }
    var project = PhotonStackProject()
    project.addAssets(from: [dark], role: .dark)
    let id = try #require(project.assets.first?.id)
    var settings = DeepSkySettings()
    settings.values["calibration.darks"] = [dark, external].map { quote($0.path) }.joined(separator: " ")
    settings.values["calibration.flats"] = ""
    settings.overrides[dark.path] = "reject"
    project.deepSkySettings = settings
    project.updateRole(for: id, role: .flat)
    #expect(project.deepSkySettings?.values["calibration.darks"] == quote(external.path))
    #expect(project.deepSkySettings?.values["calibration.flats"] == quote(dark.path))
    #expect(project.deepSkySettings?.overrides.isEmpty == true)
    project.removeAsset(id)
    #expect(project.deepSkySettings?.values["calibration.flats"] == "")
    #expect(project.deepSkySettings?.values["calibration.darks"] == quote(external.path))
}

@Test func auditBulkRemovalKeepsSurvivingReferenceAndWeight() {
    var project = auditWeightedProject()
    let retained = project.assets[1]
    project.removeAssets([project.assets[0].id, project.assets[2].id, UUID()])
    #expect(project.assets.map(\.id) == [retained.id])
    #expect(project.deepSkySettings?.values["registration.reference"] == "0")
    #expect(project.deepSkySettings?.values["stack.weights"] == "2")
}

@Test func auditAutosaveRechecksRetargetedAssetAliases() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let first = root.appendingPathComponent("first.fit"), second = root.appendingPathComponent("second.fit")
    let alias = root.appendingPathComponent("alias.fit")
    try Data([1]).write(to: first)
    try Data([2]).write(to: second)
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: second)
    var project = PhotonStackProject()
    project.addAssets(from: [first, second])
    project.assets[1].originalURL = alias
    let repository = ProjectRepository()
    let destination = root.appendingPathComponent("project")
    try await repository.autosave(project, to: destination)
    try FileManager.default.removeItem(at: alias)
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: first)
    await #expect(throws: ProjectRepositoryError.self) { try await repository.autosave(project, to: destination) }
}
