#if os(macOS)
import AppKit
import Foundation
import UniformTypeIdentifiers

@MainActor
enum NativeFilePanels {
    static let importFileExtensions = [
        "nef", "arw", "cr2", "cr3", "dng", "orf", "raf", "rw2", "srw",
        "fits", "fit", "fts",
        "tif", "tiff", "png", "jpg", "jpeg", "heic", "heif",
    ]

    static var importFileContentTypes: [UTType] {
        let fileTypes = importFileExtensions.compactMap { UTType(filenameExtension: $0) }
        return [.image] + fileTypes
    }

    static func makeImportFilesPanel(message: String) -> NSOpenPanel {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = importFileContentTypes
        panel.message = message
        return panel
    }

    static func importAssetURLs(message: String) -> [URL]? {
        let panel = makeImportFilesPanel(message: message)
        return panel.runModal() == .OK ? panel.urls : nil
    }

    static func makeRelinkAssetPanel(message: String) -> NSOpenPanel {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = importFileContentTypes
        panel.message = message
        return panel
    }

    static func relinkAssetURL(message: String) -> URL? {
        let panel = makeRelinkAssetPanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func makeImportFolderPanel(message: String) -> NSOpenPanel {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.directory]
        panel.message = message
        return panel
    }

    static func importAssetFolderURL(message: String) -> URL? {
        let panel = makeImportFolderPanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func makeOpenProjectPanel(message: String) -> NSOpenPanel {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = message
        return panel
    }

    static func openProjectDirectory(message: String) -> URL? {
        let panel = makeOpenProjectPanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func makeSaveProjectPanel(message: String) -> NSSavePanel {
        let panel = NSSavePanel()
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = "PhotonStack Project"
        panel.message = message
        return panel
    }

    static func saveProjectDirectory(message: String) -> URL? {
        let panel = makeSaveProjectPanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func makeExportImagePanel(message: String) -> NSSavePanel {
        let panel = NSSavePanel()
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = "photonstack-export.tiff"
        panel.allowedContentTypes = [.tiff, .png, .jpeg]
        panel.message = message
        return panel
    }

    static func exportImageURL(message: String) -> URL? {
        let panel = makeExportImagePanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func makeOpenCurvePresetLibraryPanel(message: String) -> NSOpenPanel {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.json]
        panel.message = message
        return panel
    }

    static func openCurvePresetLibraryURL(message: String) -> URL? {
        let panel = makeOpenCurvePresetLibraryPanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func makeExportCurvePresetLibraryPanel(message: String) -> NSSavePanel {
        let panel = NSSavePanel()
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = "photonstack-curve-presets.json"
        panel.allowedContentTypes = [.json]
        panel.message = message
        return panel
    }

    static func exportCurvePresetLibraryURL(message: String) -> URL? {
        let panel = makeExportCurvePresetLibraryPanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func makeBatchOutputDirectoryPanel(message: String) -> NSOpenPanel {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        panel.message = message
        return panel
    }

    static func openBatchOutputDirectory(message: String) -> URL? {
        let panel = makeBatchOutputDirectoryPanel(message: message)
        return panel.runModal() == .OK ? panel.url : nil
    }
}
#endif
