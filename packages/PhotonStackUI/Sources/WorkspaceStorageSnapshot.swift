import Foundation

/// Inventory of the current preview directory. Unknown files are retained, never
/// treated as reclaimable just because they share a directory with app outputs.
public struct WorkspaceStorageSnapshot: Sendable {
    public let rootURL: URL
    public let managedFileCount: Int
    public let managedBytes: Int64
    public let protectedFileCount: Int
    public let protectedBytes: Int64
    public let reclaimableFileCount: Int
    public let reclaimableBytes: Int64
    public let availableCapacityBytes: Int64?
}

struct WorkspaceStorageScan: Sendable {
    let snapshot: WorkspaceStorageSnapshot
    let reclaimableURLs: [URL]
}

enum WorkspaceStorageScanner {
    static func isManagedFileName(_ name: String) -> Bool {
        ["cache-", "artifact-marked-", "cloud-marked-", "export-layer-stack-",
         "layer-stack-", "mask-brush-", "meteor-layer-", "replay-", "restore-meteors-"]
            .contains { name.hasPrefix($0) }
    }

    static func isManagedDirectoryName(_ name: String) -> Bool {
        ["clean-sequence-", "mosaic-", "registered-batch-", "replay-", "workflow-"]
            .contains { name.hasPrefix($0) }
    }

    static func scan(root: URL, protectedPaths: Set<String>, includeUnknownFiles: Bool = true) throws -> WorkspaceStorageScan {
        let keys: Set<URLResourceKey> = [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey]
        var allCount = 0, retainedCount = 0
        var allBytes: Int64 = 0, retainedBytes: Int64 = 0, reclaimableBytes: Int64 = 0
        var reclaimable: [URL] = []
        func collect(_ directory: URL, owned: Bool, topLevel: Bool = false) throws {
            for url in try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: Array(keys)) {
                try Task.checkCancellation()
                let url = url.standardizedFileURL
                let values = try url.resourceValues(forKeys: keys)
                guard values.isSymbolicLink != true else { continue }
                if values.isDirectory == true {
                    let childOwned = owned || (topLevel && isManagedDirectoryName(url.lastPathComponent))
                    // Cleanup confirmation only needs owned candidates; do not
                    // synchronously walk potentially large unrelated folders.
                    if childOwned || includeUnknownFiles { try collect(url, owned: childOwned) }
                } else if values.isRegularFile == true {
                    let bytes = Int64(max(0, values.fileSize ?? 0))
                    allCount += 1; allBytes += bytes
                    let eligible = owned || (topLevel && isManagedFileName(url.lastPathComponent))
                    if eligible && !protectedPaths.contains(url.path) && !protectedPaths.contains(url.resolvingSymlinksInPath().path) {
                        reclaimable.append(url); reclaimableBytes += bytes
                    } else {
                        retainedCount += 1; retainedBytes += bytes
                    }
                }
            }
        }
        if FileManager.default.fileExists(atPath: root.path) { try collect(root, owned: false, topLevel: true) }
        var volume = root
        while !FileManager.default.fileExists(atPath: volume.path), volume.path != "/" { volume.deleteLastPathComponent() }
        // Match the engine's currently available filesystem capacity. The
        // important-usage estimate includes reclaimable OS storage and can be
        // materially larger than the space available to a stack right now.
        let available = (try? volume.resourceValues(forKeys: [.volumeAvailableCapacityKey]))?.volumeAvailableCapacity.map(Int64.init)
        return WorkspaceStorageScan(snapshot: WorkspaceStorageSnapshot(
            rootURL: root, managedFileCount: allCount, managedBytes: allBytes,
            protectedFileCount: retainedCount, protectedBytes: retainedBytes,
            reclaimableFileCount: reclaimable.count, reclaimableBytes: reclaimableBytes,
            availableCapacityBytes: available
        ), reclaimableURLs: reclaimable.sorted { $0.path < $1.path })
    }
}
