import Foundation

/// A snapshot of the light frames contributing to one sky pointing.
public struct MosaicPanelFolder: Codable, Equatable, Sendable {
    public var directory: URL
    public var frames: [URL]

    public static func discover(in roots: [URL]) throws -> [Self] {
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .isDirectoryKey, .isSymbolicLinkKey, .isPackageKey]
        func entries(_ directory: URL) throws -> [URL] {
            try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: Array(keys),
                                                       options: [.skipsHiddenFiles]).sorted {
                $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending
            }
        }
        func directories(_ urls: [URL]) throws -> [URL] {
            try urls.filter {
                let flags = try $0.resourceValues(forKeys: keys)
                return flags.isDirectory == true && flags.isSymbolicLink != true && flags.isPackage != true
            }
        }
        func panel(_ directory: URL, contents: [URL]) throws -> Self? {
            let lights = try directories(contents).first { $0.lastPathComponent.lowercased() == "lights" }
            let candidates = try lights.map(entries) ?? contents
            let files = try candidates.filter { url in
                guard AssetKind.detect(from: url) != .unknown else { return false }
                return try url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile == true
            }
            // Camera JPEG previews can accompany identically named FITS / RAW lights.
            let originals = Set(files.filter { [.fits, .raw].contains(AssetKind.detect(from: $0)) }
                .map { $0.deletingPathExtension().lastPathComponent.lowercased() })
            let frames = files.filter {
                let proxy = [.jpeg, .png, .heif].contains(AssetKind.detect(from: $0))
                return !proxy || !originals.contains($0.deletingPathExtension().lastPathComponent.lowercased())
            }
            return frames.isEmpty ? nil : Self(directory: directory.standardizedFileURL.resolvingSymlinksInPath(),
                                                frames: frames.map { $0.standardizedFileURL.resolvingSymlinksInPath() })
        }

        var groups: [Self] = []
        var seen = Set<URL>()
        for root in roots {
            let directory = root.resolvingSymlinksInPath().standardizedFileURL
            let contents = try entries(directory)
            let calibration = Set(WorkflowCalibrationRole.allCases.map(\.rawValue))
            let children = try directories(contents).filter { !calibration.contains($0.lastPathComponent.lowercased()) }
            var found: [Self] = []
            if !children.contains(where: { $0.lastPathComponent.lowercased() == "lights" }) {
                for child in children {
                    if let group = try panel(child, contents: entries(child)) { found.append(group) }
                }
            }
            if found.isEmpty, let group = try panel(directory, contents: contents) { found = [group] }
            guard !found.isEmpty else {
                throw WorkflowError("没有找到天区照片 / No sky panel images: \(directory.path)")
            }
            groups += found.filter { seen.insert($0.directory).inserted }
        }

        // Seestar exports a Stacked_* product beside its matching <target>_sub lights.
        let byDirectory = Dictionary(uniqueKeysWithValues: groups.map { ($0.directory.path, $0) })
        return groups.filter { group in
            let name = group.directory.lastPathComponent
            guard group.frames.allSatisfy({ $0.lastPathComponent.hasPrefix("Stacked_") }),
                  let subs = byDirectory[group.directory.deletingLastPathComponent().appendingPathComponent(name + "_sub").path],
                  subs.frames.allSatisfy({ $0.lastPathComponent.hasPrefix("Light_" + name + "_") })
            else { return true }
            return false
        }
    }
}

extension PhotonStackProject {
    /// Recover explicit light-frame directories from older flat imports. Read only
    /// project membership: opening a project must never add unselected files from disk.
    public mutating func regroupMosaicLightSequences() {
        let ordered = Set(mosaicPanelOrder)
        let recorded = Set(editGraph.operations.filter { $0.kind == .mosaic }
            .flatMap { ($0.parameters["panelIDs"] ?? "").split(separator: ",").compactMap { UUID(uuidString: String($0)) } })
        let candidates = assets.indices.filter {
            let asset = assets[$0]
            let panel = asset.role == .mosaic || (asset.role == .light && ordered.contains(asset.id))
            return panel && (asset.mosaicFolder != nil || !recorded.contains(asset.id))
        }
        var groups: [URL: [Int]] = [:]
        var masters: [Int] = []
        for index in candidates {
            let asset = assets[index]
            if let folder = asset.mosaicFolder {
                groups[folder.directory.standardizedFileURL, default: []].append(index)
                continue
            }
            let url = asset.originalURL
            let parent = url.deletingLastPathComponent().standardizedFileURL
            let name = parent.lastPathComponent
            if name.lowercased() == "lights" {
                groups[parent.deletingLastPathComponent(), default: []].append(index)
            } else if name.lowercased().hasSuffix("_sub"), url.lastPathComponent.lowercased().hasPrefix("light_") {
                groups[parent, default: []].append(index)
            } else if url.lastPathComponent.hasPrefix("Stacked_") {
                masters.append(index)
            }
        }

        var members = Set<UUID>()
        for (directory, indexes) in groups {
            let representative = indexes.first { assets[$0].mosaicFolder != nil } ?? indexes[0]
            if indexes.count == 1 && assets[representative].mosaicFolder != nil { continue }
            var seen = Set<URL>()
            let frames = indexes.flatMap { assets[$0].mosaicFolder?.frames ?? [assets[$0].originalURL] }
                .filter { seen.insert($0.standardizedFileURL).inserted }
            assets[representative].mosaicFolder = MosaicPanelFolder(directory: directory, frames: frames)
            members.formUnion(indexes.filter { $0 != representative }.map { assets[$0].id })
        }
        for index in masters {
            let url = assets[index].originalURL
            let parent = url.deletingLastPathComponent()
            let rawDirectory = parent.deletingLastPathComponent()
                .appendingPathComponent(parent.lastPathComponent + "_sub", isDirectory: true).standardizedFileURL
            if let group = groups[rawDirectory], group.contains(where: {
                assets[$0].originalURL.lastPathComponent.hasPrefix("Light_" + parent.lastPathComponent + "_")
            }) {
                members.insert(assets[index].id)
            }
        }
        guard !members.isEmpty else { return }
        // Keep every source asset and its ID for existing layers, sequences and edits.
        for index in assets.indices where members.contains(assets[index].id) { assets[index].role = .light }
        mosaicPanelOrder.removeAll { members.contains($0) }
    }
}
