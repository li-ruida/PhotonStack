import Foundation

/// Only associates the named science product with its adjacent SPCC recipe.
/// The CLI then verifies all digests before supplying the display-only TIFF.
/// This proxy never enters the edit graph or replaces the linear input.
enum SpectralCalibrationDisplayPreview {
    static func recipe(for input: URL) -> URL? {
        let source = input.resolvingSymlinksInPath()
        guard source.lastPathComponent == "calibrated.fits" else { return nil }
        let recipe = source.deletingLastPathComponent().appendingPathComponent("recipe.txt")
        guard let file = try? FileHandle(forReadingFrom: recipe) else { return nil }
        defer { try? file.close() }
        let magic = Data("PhotonStack-SPCC-1\n".utf8)
        guard (try? file.read(upToCount: magic.count)) == magic else { return nil }
        return recipe
    }
    static func cacheKey(for input: URL) -> String {
        guard let recipe = recipe(for: input) else { return "" }
        let folder = recipe.deletingLastPathComponent()
        return "|spcc-display-2|" + ["recipe.txt", "report.json", "profile.txt", "catalog.txt", "response-source-license.txt", "before.tiff", "after.tiff"].map {
            let a = try? FileManager.default.attributesOfItem(atPath: folder.appendingPathComponent($0).path)
            let modified = (a?[.modificationDate] as? Date ?? .distantPast).timeIntervalSince1970
            return "\($0):\(a?[.size] ?? 0):\(modified):\(a?[.systemFileNumber] ?? 0)"
        }.joined(separator: "|")
    }
}
