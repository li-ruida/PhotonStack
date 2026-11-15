import Foundation

/// A snapshot of the light frames contributing to one sky pointing.
public struct MosaicPanelFolder: Codable, Equatable, Sendable {
    public var directory: URL
    public var frames: [URL]

    /// Retain selection order while removing duplicate links and camera-generated display proxies.
    static func scienceFrames(in files: [URL]) -> [URL] {
        let originals = Set(files.filter { [.fits, .raw].contains(AssetKind.detect(from: $0)) }
            .map { $0.deletingPathExtension().standardizedFileURL.path.lowercased() })
        var seen = Set<String>()
        return files.filter {
            let kind = AssetKind.detect(from: $0)
            if [.jpeg, .heif, .png].contains(kind),
               originals.contains($0.deletingPathExtension().standardizedFileURL.path.lowercased()) { return false }
            return seen.insert($0.standardizedFileURL.path).inserted
        }
    }

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
            let frames = scienceFrames(in: files.map { $0.standardizedFileURL.resolvingSymlinksInPath() })
            return frames.isEmpty ? nil : Self(directory: directory.standardizedFileURL.resolvingSymlinksInPath(),
                                                frames: frames)
        }

        var groups: [Self] = []
        var seen = Set<URL>()
        for root in roots {
            let directory = root.resolvingSymlinksInPath().standardizedFileURL
            let contents = try entries(directory)
            let calibration = Set(WorkflowCalibrationRole.allCases.map(\.rawValue))
            let children = try directories(contents).filter { !calibration.contains($0.lastPathComponent.lowercased()) }
            var found: [Self] = []
            let direct = try panel(directory, contents: contents)
            // An explicit light sequence can also have a previews/ or exports/ subfolder.
            if let direct, children.contains(where: { $0.lastPathComponent.lowercased() == "lights" })
                || direct.frames.contains(where: { $0.lastPathComponent.lowercased().hasPrefix("light_") })
                || roots.count > 1 && direct.frames.contains(where: { [.raw, .fits].contains(AssetKind.detect(from: $0)) }) {
                found = [direct]
            } else {
                for child in children {
                    if let group = try panel(child, contents: entries(child)) { found.append(group) }
                }
            }
            if let direct, found.isEmpty || (direct.frames.contains { [.raw, .fits].contains(AssetKind.detect(from: $0)) }
                && found.allSatisfy { $0.frames.allSatisfy { [.jpeg, .heif, .png].contains(AssetKind.detect(from: $0)) } }) {
                found = [direct]
            }
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
    /// Older operations stored panel IDs only. Freeze their last saved membership
    /// before a subsequent import can change which lights belong to those IDs.
    public mutating func preserveLegacyMosaicSelections() {
        for index in editGraph.operations.indices {
            let operation = editGraph.operations[index]
            guard operation.kind == .mosaic, operation.parameters["panelSources"] == nil,
                  let recorded = operation.parameters["panelIDs"] else { continue }
            let ids = Set(recorded.split(separator: ",").compactMap { UUID(uuidString: String($0)) })
            let panels = assets.filter { ids.contains($0.id) }
            if !panels.isEmpty && panels.count == ids.count {
                editGraph.operations[index].parameters["panelSources"] = try? MosaicPanelFolder.encodeSelection(panels)
            }
        }
    }

    /// Recover explicit light-frame directories from older flat imports. Read only
    /// project membership: opening a project must never add unselected files from disk.
    public mutating func regroupMosaicLightSequences() {
        preserveLegacyMosaicSelections()
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
            let frames = MosaicPanelFolder.scienceFrames(in: indexes.flatMap {
                assets[$0].mosaicFolder?.frames ?? [assets[$0].originalURL]
            })
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

extension MosaicPanelFolder {
    public static func encodeSelection(_ panels: [PhotonStackAsset]) throws -> String {
        var folders: [String: MosaicPanelFolder] = [:]
        for panel in panels { folders[panel.id.uuidString] = panel.mosaicFolder }
        return try encodeFolders(folders)
    }

    static func encodeFolders(_ folders: [String: MosaicPanelFolder]) throws -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return String(decoding: try encoder.encode(folders), as: UTF8.self)
    }

    public static func decodeSelection(_ value: String) throws -> [String: MosaicPanelFolder] {
        try JSONDecoder().decode([String: MosaicPanelFolder].self, from: Data(value.utf8))
    }
}
