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
