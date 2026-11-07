import Foundation
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
@testable import PhotonStackUI

private let exportProtectionPNG = Data(base64Encoded: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")!

private func exportProtectionService(_ root: URL) throws -> CLIProcessingService {
    let executable = root.appendingPathComponent("engine")
    let script = """
    #!/bin/sh
    printf invoked > "$PWD/launched"
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then output="$2"; shift; fi
      shift
    done
    cp "$PWD/selected.png" "$output"
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    return CLIProcessingService(executableURL: executable)
}

@Test(arguments: ["asset", "settings-calibration", "recipe-light", "recipe-calibration", "source-master"],
      ["direct", "symlink", "parent-symlink", "hardlink"]) @MainActor
func exportRejectsProjectSourcesAndTheirAliases(reference: String, aliasKind: String) async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackExportProtection-\(UUID().uuidString)")
    let sourceDirectory = root.appendingPathComponent("sources")
    try FileManager.default.createDirectory(at: sourceDirectory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let selected = root.appendingPathComponent("selected.png")
    let source = sourceDirectory.appendingPathComponent("protected.tiff")
    let original = Data("protected original contents".utf8)
    try exportProtectionPNG.write(to: selected)
    try original.write(to: source)
    func quote(_ text: String) -> String { "\"" + text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\"" }
    var project = PhotonStackProject(name: "Export protection")
    project.addAssets(from: [selected])
    switch reference {
    case "asset": project.addAssets(from: [source], role: .dark)
    case "settings-calibration":
        var settings = DeepSkySettings(); settings.values["calibration.darks"] = quote(source.path)
        project.deepSkySettings = settings
    case "recipe-light", "recipe-calibration":
        let line = reference == "recipe-light" ? "input \(quote(source.path))" : "calibration.darks \(quote(quote(source.path)))"
        project.editGraph = EditGraph(operations: [EditOperation(kind: .stack,
            parameters: ["mode": "deepSkyRecipe", "recipe": "PHOTONSTACK_DEEP_SKY_RECIPE 1\n\(line)\n"], isEnabled: false)])
    default:
        project.editGraph = EditGraph(operations: [EditOperation(kind: .stack, parameters: ["sourceMaster": source.path], isEnabled: false)])
    }
    let destination: URL
    switch aliasKind {
    case "symlink":
        destination = root.appendingPathComponent("alias.tiff")
        try FileManager.default.createSymbolicLink(at: destination, withDestinationURL: source)
    case "parent-symlink":
        let alias = root.appendingPathComponent("source-alias")
        try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: sourceDirectory)
        destination = alias.appendingPathComponent(source.lastPathComponent)
    case "hardlink":
        destination = root.appendingPathComponent("hardlink.tiff")
        try FileManager.default.linkItem(at: source, to: destination)
    default: destination = source
    }
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: try exportProtectionService(root), previewDirectory: root.appendingPathComponent("previews"))
    await model.exportCurrentImage(to: destination)
    #expect(model.errorMessage?.contains("cannot overwrite") == true)
    #expect(model.jobs.isEmpty)
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("launched").path))
    #expect(try Data(contentsOf: source) == original)
    #expect(try Data(contentsOf: selected) == exportProtectionPNG)
}

@Test @MainActor func exportPreservesReferencedSidecarAndAllowsOrdinaryOutputOverwrite() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackExportSidecarProtection-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let selected = root.appendingPathComponent("selected.png"), output = root.appendingPathComponent("export.tiff")
    let sidecar = root.appendingPathComponent("export.tiff.photonstack.json")
    try exportProtectionPNG.write(to: selected)
    try Data("original metadata".utf8).write(to: sidecar)
    var project = PhotonStackProject(name: "Sidecar protection")
    project.addAssets(from: [selected, sidecar])
    let service = try exportProtectionService(root)
    let protected = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: service,
        previewDirectory: root.appendingPathComponent("previews"))
    await protected.exportCurrentImage(to: output)
    #expect(protected.errorMessage?.contains("cannot overwrite") == true)
    #expect(!FileManager.default.fileExists(atPath: output.path))
    #expect(try String(contentsOf: sidecar, encoding: .utf8) == "original metadata")
    var ordinaryProject = PhotonStackProject(name: "Ordinary overwrite")
    ordinaryProject.addAssets(from: [selected])
    let ordinary = PhotonStackWorkspaceModel(project: ordinaryProject, autosaveEnabled: false, processingService: service,
        previewDirectory: root.appendingPathComponent("ordinary-previews"))
    try Data("previous export".utf8).write(to: output)
    await ordinary.exportCurrentImage(to: output)
    #expect(ordinary.errorMessage == nil)
    #expect(try Data(contentsOf: output) == exportProtectionPNG)
    #expect(try Data(contentsOf: selected) == exportProtectionPNG)
}
